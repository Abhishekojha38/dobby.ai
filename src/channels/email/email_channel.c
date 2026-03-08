/*
 * email_channel.c — IMAP-poll / SMTP-reply email channel for Dobby
 *
 * IMAP poll loop:
 *   1. Connect to IMAP server, search UNSEEN messages in INBOX
 *   2. For each unseen UID: fetch headers + body, mark as SEEN
 *   3. Extract From: address → chat_id, Subject:, plain-text body
 *   4. Publish inbound_msg_t to bus ("email", from_addr, from_addr, body, NULL)
 *   5. Worker replies via outbound queue → email_channel_send() fires SMTP
 *
 * Synchronous path is NOT used here (no response_pair) because email is
 * inherently async — the response arrives seconds after the request.
 *
 * SMTP reply:
 *   Re-uses the Subject: from the original email (prefixed with "Re: [Dobby] ")
 *   Sends plain-text reply from our address to the original From: address.
 */
#include "email_channel.h"
#include "../../core/log.h"
#include "../../core/config.h"
#include "../../channels/channel.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

/* ── Config limits ─────────────────────────────────────────────── */
#define EMAIL_ADDR_MAX   256
#define EMAIL_SUBJ_MAX   512
#define EMAIL_BODY_MAX   16384
#define EMAIL_UID_MAX    32

/* ── Internal state ────────────────────────────────────────────── */
struct email_channel {
    /* config */
    char imap_url[512];
    char smtp_url[512];
    char address[EMAIL_ADDR_MAX];
    char password[256];
    char inbox[128];
    char subject_tag[128];
    int  poll_interval;

    /* runtime */
    bus_t          *bus;
    allowlist_t    *allowlist;        /* NULL = no enforcement */
    channel_t       channel_iface;   /* registered in channel registry */
    pthread_t       poll_thread;
    volatile bool   running;

    /* last seen UID (simple seen-tracking — resets on restart) */
    unsigned long   last_uid;

    /* Per-sender reply context — stored when message arrives, used when replying.
     * Keyed by from-address (one entry, last sender wins for now).
     * Good enough for a personal bot with one primary user. */
    char   last_from[EMAIL_ADDR_MAX];      /* who sent the last email      */
    char   last_subject[EMAIL_SUBJ_MAX];   /* their Subject: line          */
    char   last_message_id[512];           /* their Message-ID: header     */
};

/* ── curl write-callback ───────────────────────────────────────── */
typedef struct { char *data; size_t size; } membuf_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    membuf_t *m = (membuf_t *)ud;
    size_t total = size * nmemb;
    char *tmp = realloc(m->data, m->size + total + 1);
    if (!tmp) return 0;
    m->data = tmp;
    memcpy(m->data + m->size, ptr, total);
    m->size += total;
    m->data[m->size] = '\0';
    return total;
}

/* ── Header extraction helpers ─────────────────────────────────── */

/* Extract value of a header field from raw RFC 2822 message.
 * Returns a heap-allocated string or NULL. Caller frees. */
static char *extract_header(const char *msg, const char *field) {
    if (!msg || !field) return NULL;
    size_t flen = strlen(field);
    const char *p = msg;
    while (*p) {
        if (strncasecmp(p, field, flen) == 0 && p[flen] == ':') {
            const char *v = p + flen + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *end = strchr(v, '\n');
            if (!end) end = v + strlen(v);
            /* Handle folded headers */
            size_t len = (size_t)(end - v);
            while (len > 0 && (v[len-1] == '\r' || v[len-1] == ' ')) len--;
            char *result = strndup(v, len);
            return result;
        }
        /* Advance to next line */
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return NULL;
}

/* Trim trailing whitespace in-place, return new length. */
static size_t rtrim(const char *s, size_t len) {
    while (len > 0 && (s[len-1] == '\r' || s[len-1] == '\n' || s[len-1] == ' '))
        len--;
    return len;
}

/* Extract plain-text body from a raw RFC 2822 message.
 *
 * Handles two common structures:
 *
 * 1. Simple (Content-Type: text/plain):
 *      headers\r\n\r\nbody text
 *
 * 2. Multipart/alternative (Gmail web composer default):
 *      Content-Type: multipart/alternative; boundary="abc"
 *      \r\n
 *      --abc\r\n
 *      Content-Type: text/plain; charset="UTF-8"\r\n
 *      \r\n
 *      body text here
 *      --abc\r\n
 *      Content-Type: text/html; ...
 *      ...
 *      --abc--
 *
 * Strategy:
 *   - If Content-Type header contains "multipart" → find boundary →
 *     scan parts → return the first text/plain part body.
 *   - Otherwise → return everything after the header blank line.
 *
 * Does NOT handle base64 or quoted-printable transfer encoding.
 * That requires GMime — acceptable for now since most plain emails
 * from Gmail are UTF-8 with no transfer encoding on the text/plain part.
 */
static char *extract_body(const char *msg) {
    if (!msg) return NULL;

    /* ── Check if this is multipart ──────────────────────────────── */
    char *ct = extract_header(msg, "Content-Type");
    bool is_multipart = ct && strstr(ct, "multipart");

    if (is_multipart) {
        /* Extract boundary value from:  boundary="abc123"  or  boundary=abc123 */
        const char *bp = strstr(ct, "boundary");
        char boundary[256] = {0};
        if (bp) {
            bp += 8; /* skip "boundary" */
            while (*bp == ' ' || *bp == '=') bp++;
            bool quoted = (*bp == '"');
            if (quoted) bp++;
            size_t bi = 0;
            while (*bp && bi < sizeof(boundary)-3) {
                if (quoted && *bp == '"') break;
                if (!quoted && (*bp == ';' || *bp == '\r' || *bp == '\n')) break;
                boundary[bi++] = *bp++;
            }
            /* MIME boundary in message is prefixed with "--" */
            memmove(boundary + 2, boundary, bi + 1);
            boundary[0] = '-'; boundary[1] = '-';
        }
        free(ct);

        if (!boundary[2]) {
            /* No boundary found — fall through to simple extraction */
            goto simple;
        }

        /* Scan parts looking for text/plain */
        const char *p = msg;
        while ((p = strstr(p, boundary)) != NULL) {
            p += strlen(boundary);
            /* Skip optional \r\n after boundary marker */
            if (*p == '\r') p++;
            if (*p == '\n') p++;
            if (*p == '-') break; /* closing boundary "--" */

            /* Each part has its own headers + blank line + body */
            const char *part_start = p;
            char *part_ct = extract_header(part_start, "Content-Type");
            bool is_plain = part_ct && strncasecmp(part_ct, "text/plain", 10) == 0;
            free(part_ct);

            /* Skip part headers to find part body */
            const char *part_body = strstr(part_start, "\r\n\r\n");
            if (!part_body) part_body = strstr(part_start, "\n\n");
            if (part_body) {
                part_body += (part_body[0] == '\r') ? 4 : 2;
            } else {
                continue;
            }

            if (is_plain) {
                /* Find end of this part (next boundary) */
                const char *end = strstr(part_body, boundary);
                size_t len = end ? (size_t)(end - part_body) : strlen(part_body);
                len = rtrim(part_body, len);
                if (len > EMAIL_BODY_MAX) len = EMAIL_BODY_MAX;
                return strndup(part_body, len);
            }
        }
        /* No text/plain part found — return empty */
        return strdup("(no plain text body)");
    }

    free(ct);

simple:;
    /* Simple message — find blank line separator */
    const char *body = strstr(msg, "\r\n\r\n");
    if (body) body += 4;
    else {
        body = strstr(msg, "\n\n");
        if (body) body += 2;
        else return strdup(msg);
    }
    size_t len = rtrim(body, strlen(body));
    if (len > EMAIL_BODY_MAX) len = EMAIL_BODY_MAX;
    return strndup(body, len);
}

/* Extract email address from "Name <addr>" or bare "addr". */
static char *extract_addr(const char *from) {
    if (!from) return NULL;
    const char *lt = strchr(from, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt && gt > lt + 1)
        return strndup(lt + 1, (size_t)(gt - lt - 1));
    /* Bare address */
    char *copy = strdup(from);
    /* Strip trailing whitespace */
    for (char *p = copy + strlen(copy) - 1; p >= copy && isspace((unsigned char)*p); p--)
        *p = '\0';
    return copy;
}

/* ── IMAP: search UNSEEN messages ──────────────────────────────── */

static char *imap_search_unseen(email_channel_t *ec) {
    /* URL format: imaps://host:port/INBOX  (no query string — space is illegal)
     * The SEARCH command is issued via CURLOPT_CUSTOMREQUEST instead. */
    char url[600];
    snprintf(url, sizeof(url), "%s/%s", ec->imap_url, ec->inbox);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    membuf_t buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, ec->address);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, ec->password);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    /* UID SEARCH UNSEEN returns UIDs, not sequence numbers.
     * This is critical — sequence numbers change as messages are deleted,
     * but UIDs are stable. Without UID prefix, SEARCH returns sequence
     * numbers which differ from UIDs on Gmail and most real servers. */
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "UID SEARCH UNSEEN");

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        LOG_WARN("Email IMAP search failed: %s", curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    return buf.data;   /* caller frees */
}

/* ── IMAP: fetch a single message by UID ───────────────────────── */

static char *imap_fetch_message(email_channel_t *ec, unsigned long uid, size_t *out_size) {
    /* Fetch a message by UID using libcurl's native IMAP URL form.
     *
     * The URL format is:
     *   imaps://imap.gmail.com:993/INBOX/;UID=N
     *
     * This makes libcurl issue:  UID FETCH N (BODY.PEEK[])
     * and — critically — delivers the message body bytes to the write
     * callback.  CUSTOMREQUEST does NOT do this: it delivers only the
     * untagged response line (* N FETCH ...) and discards the literal body.
     *
     * The earlier comment claiming ";UID=N issues a sequence-number FETCH"
     * was wrong.  libcurl's IMAP URL handler always issues UID FETCH when
     * the URL contains ;UID=.  Gmail UIDs work correctly with this form. */
    char url[600];
    snprintf(url, sizeof(url), "%s/%s/;UID=%lu", ec->imap_url, ec->inbox, uid);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    membuf_t buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, ec->address);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, ec->password);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        LOG_WARN("Email IMAP fetch UID=%lu failed: %s", uid, curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    if (out_size) *out_size = buf.size;
    return buf.data;
}

/* ── SMTP: send reply ───────────────────────────────────────────── */

/* curl reads the email payload from this callback */
typedef struct { const char *data; size_t pos; size_t len; } smtp_read_ctx_t;

static size_t smtp_read_cb(char *buf, size_t size, size_t nmemb, void *ud) {
    smtp_read_ctx_t *ctx = (smtp_read_ctx_t *)ud;
    size_t avail = size * nmemb;
    size_t remain = ctx->len - ctx->pos;
    if (remain == 0) return 0;
    size_t n = remain < avail ? remain : avail;
    memcpy(buf, ctx->data + ctx->pos, n);
    ctx->pos += n;
    return n;
}

static bool smtp_send(email_channel_t *ec,
                      const char *to_addr,
                      const char *subject,
                      const char *body,
                      const char *in_reply_to) {
    /* Generate a unique Message-ID for this outbound message */
    char msg_id[128];
    snprintf(msg_id, sizeof(msg_id), "<%lu.%s>", (unsigned long)time(NULL), ec->address);

    /* Build RFC 2822 payload with threading headers */
    char payload[EMAIL_BODY_MAX + 2048];
    int plen;
    if (in_reply_to && *in_reply_to) {
        /* Reply: include In-Reply-To and References so Gmail threads correctly */
        plen = snprintf(payload, sizeof(payload),
            "From: Dobby <%s>\r\n"
            "To: <%s>\r\n"
            "Subject: %s\r\n"
            "Message-ID: %s\r\n"
            "In-Reply-To: %s\r\n"
            "References: %s\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "\r\n"
            "%s\r\n",
            ec->address, to_addr, subject,
            msg_id, in_reply_to, in_reply_to,
            body);
    } else {
        plen = snprintf(payload, sizeof(payload),
            "From: Dobby <%s>\r\n"
            "To: <%s>\r\n"
            "Subject: %s\r\n"
            "Message-ID: %s\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "\r\n"
            "%s\r\n",
            ec->address, to_addr, subject, msg_id, body);
    }
    if (plen <= 0) return false;

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist *rcpt = curl_slist_append(NULL, to_addr);
    smtp_read_ctx_t rctx = { .data = payload, .pos = 0, .len = (size_t)plen };

    curl_easy_setopt(curl, CURLOPT_URL, ec->smtp_url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, ec->address);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, ec->password);
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, ec->address);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpt);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, smtp_read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA, &rctx);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(rcpt);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        LOG_WARN("SMTP send to %s failed: %s", to_addr, curl_easy_strerror(rc));
        return false;
    }
    LOG_INFO("Email sent to %s", to_addr);
    return true;
}

/* ── channel_t send() callback ─────────────────────────────────── */

static void email_send(channel_t *self, outbound_msg_t *msg) {
    email_channel_t *ec = (email_channel_t *)self->priv;
    if (!msg || !msg->chat_id || !msg->content) return;

    /* Outbound allowlist — chat_id is the recipient address */
    if (ec->allowlist && !allowlist_check(ec->allowlist, ACL_EMAIL, msg->chat_id)) {
        LOG_WARN("Email reply blocked: recipient '%s' not in allowlist", msg->chat_id);
        return;
    }

    /* Use the original subject if we have it, otherwise fall back to tag */
    char subject[EMAIL_SUBJ_MAX];
    if (ec->last_subject[0]) {
        /* Prepend "Re: " unless already present */
        if (strncasecmp(ec->last_subject, "re:", 3) == 0)
            snprintf(subject, sizeof(subject), "%s", ec->last_subject);
        else
            snprintf(subject, sizeof(subject), "Re: %s", ec->last_subject);
    } else {
        snprintf(subject, sizeof(subject), "Re: %s", ec->subject_tag);
    }

    /* Pass Message-ID of original email for threading */
    const char *in_reply_to = ec->last_message_id[0] ? ec->last_message_id : NULL;

    smtp_send(ec, msg->chat_id, subject, msg->content, in_reply_to);
}

/* ── Poll thread ───────────────────────────────────────────────── */

static void process_uid(email_channel_t *ec, unsigned long uid) {
    size_t raw_size = 0;
    char *raw = imap_fetch_message(ec, uid, &raw_size);
    if (!raw) return;

    /* Dump first 800 chars of raw IMAP response at debug level so we can
     * diagnose envelope-stripping and body-extraction issues. */
    LOG_DEBUG("Email UID=%lu raw_size=%zu (first 800): %.800s", uid, raw_size, raw);

    /* With the native ;UID=N URL form, libcurl delivers the raw RFC 2822
     * message directly to the write callback — no IMAP envelope wrapper.
     * raw starts directly with the message headers (Delivered-To:, From:, etc.)
     * No stripping needed; just use raw as-is. */
    const char *msg = raw;

    LOG_DEBUG("Email UID=%lu after strip (first 400): %.400s", uid, msg);
    LOG_DEBUG("Email UID=%lu after strip first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
              uid,
              (unsigned char)msg[0], (unsigned char)msg[1],
              (unsigned char)msg[2], (unsigned char)msg[3],
              (unsigned char)msg[4], (unsigned char)msg[5],
              (unsigned char)msg[6], (unsigned char)msg[7]);

    char *from_raw    = extract_header(msg, "From");
    char *from        = from_raw ? extract_addr(from_raw) : NULL;
    char *subject_raw = extract_header(msg, "Subject");
    char *message_id  = extract_header(msg, "Message-ID");
    char *body        = extract_body(msg);

    LOG_DEBUG("Email UID=%lu from='%s' body='%.200s'",
              uid, from ? from : "(null)", body ? body : "(null)");

    free(from_raw);
    free(raw);   /* raw is the heap allocation; msg points inside it */

    if (!from || !body || !*from || !*body) {
        LOG_DEBUG("Email UID=%lu: empty from/body, skipping", uid);
        free(from); free(body); free(subject_raw); free(message_id);
        return;
    }

    /* Store reply context — used by email_send() to set subject + threading headers */
    strncpy(ec->last_from,       from,                         sizeof(ec->last_from)       - 1);
    strncpy(ec->last_subject,    subject_raw ? subject_raw : "", sizeof(ec->last_subject)   - 1);
    strncpy(ec->last_message_id, message_id  ? message_id  : "", sizeof(ec->last_message_id)- 1);
    free(subject_raw);
    free(message_id);

    /* ── Inbound allowlist check ──────────────────────────────────
     * If [email] section exists in allowlist.conf, only accept mail
     * from listed addresses/domains. Drop everything else silently. */
    if (ec->allowlist && !allowlist_check(ec->allowlist, ACL_EMAIL, from)) {
        LOG_WARN("Email UID=%lu: sender '%s' not in allowlist — ignored", uid, from);
        free(from); free(body);
        return;
    }

    LOG_INFO("Email from=%s uid=%lu subject='%s' len=%zu",
             from, uid, ec->last_subject, strlen(body));

    inbound_msg_t *inbound = inbound_msg_new("email", from, from, body, NULL);
    free(body);
    free(from);

    if (inbound) bus_publish_inbound(ec->bus, inbound);
}

static void *poll_fn(void *arg) {
    email_channel_t *ec = (email_channel_t *)arg;
    LOG_INFO("Email poll thread started, interval=%ds", ec->poll_interval);

    int poll_count = 0;
    while (ec->running) {
        poll_count++;
        LOG_DEBUG("Email poll #%d — checking for new messages", poll_count);

        char *result = imap_search_unseen(ec);
        if (result && *result) {
            /* Parse UID list: "* SEARCH 1 2 3\r\n" or just "1 2 3" */
            char *p = result;
            /* Skip "* SEARCH " prefix if present */
            if (strncasecmp(p, "* SEARCH", 8) == 0) p += 8;
            while (*p == ' ') p++;

            /* Count new UIDs first for the log */
            int new_count = 0;
            char *scan = strdup(p);
            char *tok2 = strtok(scan, " \r\n");
            while (tok2) {
                unsigned long u = strtoul(tok2, NULL, 10);
                if (u > 0 && u > ec->last_uid) new_count++;
                tok2 = strtok(NULL, " \r\n");
            }
            free(scan);

            if (new_count > 0)
                LOG_INFO("Email poll #%d — %d new message(s)", poll_count, new_count);
            else
                LOG_DEBUG("Email poll #%d — 0 new messages", poll_count);

            char *tok = strtok(p, " \r\n");
            while (tok) {
                unsigned long uid = strtoul(tok, NULL, 10);
                if (uid > 0 && uid > ec->last_uid) {
                    process_uid(ec, uid);
                    if (uid > ec->last_uid) ec->last_uid = uid;
                }
                tok = strtok(NULL, " \r\n");
            }
        } else {
            LOG_DEBUG("Email poll #%d — inbox empty or no response", poll_count);
        }
        free(result);

        /* Sleep in 1-second chunks so we respond to stop quickly */
        for (int s = 0; s < ec->poll_interval && ec->running; s++)
            sleep(1);
    }

    LOG_INFO("Email poll thread stopped");
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────── */

email_channel_t *email_channel_create(config_t *cfg, bus_t *bus, allowlist_t *al) {
    if (!cfg || !bus) return NULL;

    /* imap_url is required — if missing, email channel is disabled */
    const char *imap = config_get(cfg, "email", "imap_url", NULL);
    if (!imap || !*imap) {
        LOG_INFO("Email channel: no imap_url in config — disabled");
        return NULL;
    }

    email_channel_t *ec = calloc(1, sizeof(*ec));
    if (!ec) return NULL;

    strncpy(ec->imap_url, imap, sizeof(ec->imap_url) - 1);

    const char *smtp = config_get(cfg, "email", "smtp_url", NULL);
    if (smtp) strncpy(ec->smtp_url, smtp, sizeof(ec->smtp_url) - 1);

    const char *addr = config_get(cfg, "email", "address", NULL);
    if (addr) strncpy(ec->address, addr, sizeof(ec->address) - 1);

    /* Password: EMAIL_PASSWORD env > config */
    const char *pw = getenv("EMAIL_PASSWORD");
    if (!pw || !*pw) pw = config_get(cfg, "email", "password", "");
    if (pw) strncpy(ec->password, pw, sizeof(ec->password) - 1);

    const char *inbox = config_get(cfg, "email", "inbox", "INBOX");
    strncpy(ec->inbox, inbox, sizeof(ec->inbox) - 1);

    const char *tag = config_get(cfg, "email", "subject_tag", "[Dobby]");
    strncpy(ec->subject_tag, tag, sizeof(ec->subject_tag) - 1);

    ec->poll_interval = config_get_int(cfg, "email", "poll_interval", 60);
    if (ec->poll_interval < 5) ec->poll_interval = 5;

    ec->bus       = bus;
    ec->allowlist = al;   /* NULL = no enforcement */

    /* Register as a channel so the dispatcher can route outbound replies */
    strncpy(ec->channel_iface.name, "email", CHANNEL_NAME_MAX - 1);
    ec->channel_iface.send = email_send;
    ec->channel_iface.priv = ec;
    channel_register(&ec->channel_iface);

    LOG_INFO("Email channel configured: imap=%s addr=%s poll=%ds",
              ec->imap_url, ec->address, ec->poll_interval);
    return ec;
}

bool email_channel_start(email_channel_t *ch) {
    if (!ch) return false;
    ch->running = true;
    if (pthread_create(&ch->poll_thread, NULL, poll_fn, ch) != 0) {
        LOG_WARN("Email: failed to start poll thread");
        ch->running = false;
        return false;
    }
    LOG_INFO("Email channel started");
    return true;
}

void email_channel_destroy(email_channel_t *ch) {
    if (!ch) return;
    ch->running = false;
    pthread_join(ch->poll_thread, NULL);
    free(ch);
}

void email_channel_status(email_channel_t *ch, char *buf, size_t buf_size) {
    if (!ch || !buf) return;
    snprintf(buf, buf_size,
        "  Address      : %s\n"
        "  IMAP         : %s\n"
        "  SMTP         : %s\n"
        "  Inbox        : %s\n"
        "  Poll interval: %ds\n"
        "  Last UID seen: %lu\n"
        "  Last sender  : %s\n"
        "  Last subject : %s\n"
        "  Running      : %s\n",
        ch->address,
        ch->imap_url,
        ch->smtp_url,
        ch->inbox,
        ch->poll_interval,
        ch->last_uid,
        ch->last_from[0]    ? ch->last_from    : "(none)",
        ch->last_subject[0] ? ch->last_subject : "(none)",
        ch->running         ? "yes"            : "no");
}

bool email_channel_is_allowed(email_channel_t *ch, const char *address) {
    if (!ch || !address) return false;
    if (!ch->allowlist) return true;   /* no allowlist = allow all */
    return allowlist_check(ch->allowlist, ACL_EMAIL, address);
}

bool email_channel_send_direct(email_channel_t *ch,
                                const char *to,
                                const char *subject,
                                const char *body) {
    if (!ch || !to || !subject || !body) return false;

    /* ── Outbound allowlist check ─────────────────────────────────
     * Refuse to send to addresses not in the [email] allowlist. */
    if (ch->allowlist && !allowlist_check(ch->allowlist, ACL_EMAIL, to)) {
        LOG_WARN("Email send blocked: recipient '%s' not in allowlist", to);
        return false;
    }

    /* Proactive send — no threading context */
    return smtp_send(ch, to, subject, body, NULL);
}
