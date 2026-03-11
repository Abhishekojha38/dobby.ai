/*
 * whatsapp_channel.c — WhatsApp channel for Dobby
 *
 * Connects to the wa-bridge Node.js sidecar over a plain TCP socket.
 * The bridge sends newline-delimited JSON frames:
 *
 *   Inbound:  {"type":"message","sender":"...@s.whatsapp.net","pn":"...","content":"..."}
 *             {"type":"status","status":"connected"|"disconnected"}
 *
 *   Outbound: {"type":"send","to":"...@s.whatsapp.net","text":"..."}
 *
 * Each sender phone number gets its own Dobby session: whatsapp:<pn>
 */
#include "whatsapp_channel.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"
#include "../../bus/bus.h"
#include "../../channels/channel.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define READ_BUF   4096
#define LINE_MAX   8192

struct whatsapp_channel {
    char         host[128];
    int          port;
    char         token[256];    /* empty = no auth */
    bus_t       *bus;
    allowlist_t *allowlist;
    int          sock;          /* -1 = not connected */
    pthread_mutex_t send_lock;
    pthread_t    thread;
    bool         running;
    channel_t    channel_iface; /* registered in channel registry */
};

/* ── Helpers ────────────────────────────────────────────────────── */

/* Normalise "12345" → "12345@s.whatsapp.net" */
static void ensure_jid(const char *in, char *out, size_t out_sz) {
    if (strchr(in, '@')) {
        snprintf(out, out_sz, "%s", in);
    } else {
        snprintf(out, out_sz, "%s@s.whatsapp.net", in);
    }
}

/* Send a raw JSON line to the bridge (mutex-protected). */
static bool send_line(whatsapp_channel_t *ch, const char *json) {
    if (ch->sock < 0) return false;
    pthread_mutex_lock(&ch->send_lock);
    bool ok = true;
    size_t len = strlen(json);
    if (write(ch->sock, json, len) < 0 || write(ch->sock, "\n", 1) < 0) {
        LOG_WARN("WhatsApp: send failed: %s", strerror(errno));
        ok = false;
    }
    pthread_mutex_unlock(&ch->send_lock);
    return ok;
}

/* ── Connection ─────────────────────────────────────────────────── */

static int tcp_connect(const char *host, int port) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        /* Try hostname resolution */
        struct hostent *he = gethostbyname(host);
        if (!he) { LOG_WARN("WhatsApp: cannot resolve %s", host); return -1; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

/* ── Message handling ───────────────────────────────────────────── */

static void handle_message(whatsapp_channel_t *ch, cJSON *obj) {
    const char *sender   = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "sender"));
    const char *reply_jid= cJSON_GetStringValue(cJSON_GetObjectItem(obj, "replyJid"));
    const char *pn       = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "pn"));
    const char *content  = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "content"));

    if (!sender || !content) return;

    const char *user_id = (pn && *pn) ? pn : sender;

    if (ch->allowlist && !allowlist_check(ch->allowlist, ACL_EMAIL, user_id)) {
        LOG_WARN("WhatsApp: message from '%s' blocked by allowlist", user_id);
        return;
    }

    /* session key: whatsapp:<pn> */
    char chat_id[128];
    snprintf(chat_id, sizeof(chat_id), "whatsapp:%s", user_id);

    /* Pass replyJid as metadata so dispatcher uses it verbatim for replies.
     * Format: "replyJid=<jid>" stored in the extra field of inbound_msg_t. */
    char meta[256] = "";
    if (reply_jid && *reply_jid)
        snprintf(meta, sizeof(meta), "replyJid=%s", reply_jid);

    inbound_msg_t *msg = inbound_msg_new("whatsapp", user_id, chat_id, content,
                                          meta[0] ? meta : NULL);
    if (!msg) return;

    if (!bus_publish_inbound(ch->bus, msg)) {
        LOG_WARN("WhatsApp: bus rejected message from %s", user_id);
    }
}

static void handle_status(const char *status) {
    LOG_DEBUG("WhatsApp bridge status: %s", status);
    if (strcmp(status, "connected") == 0)
        printf("[whatsapp] ✅ Bridge connected\n");
    else if (strcmp(status, "disconnected") == 0)
        printf("[whatsapp] ⚠️  Bridge disconnected\n");
}

/* ── Receive thread ─────────────────────────────────────────────── */

static void *recv_thread(void *arg) {
    whatsapp_channel_t *ch = (whatsapp_channel_t *)arg;
    char raw[READ_BUF];
    char line[LINE_MAX];
    size_t line_len = 0;

    while (ch->running) {
        /* Connect / reconnect */
        if (ch->sock < 0) {
            LOG_DEBUG("WhatsApp: connecting to %s:%d", ch->host, ch->port);
            ch->sock = tcp_connect(ch->host, ch->port);
            if (ch->sock < 0) {
                LOG_WARN("WhatsApp: bridge not reachable, retry in 5s");
                sleep(5);
                continue;
            }
            LOG_DEBUG("WhatsApp: TCP connected");

            /* Send auth token if configured */
            if (ch->token[0]) {
                cJSON *auth = cJSON_CreateObject();
                cJSON_AddStringToObject(auth, "type",  "auth");
                cJSON_AddStringToObject(auth, "token", ch->token);
                char *js = cJSON_PrintUnformatted(auth);
                cJSON_Delete(auth);
                if (js) { send_line(ch, js); free(js); }
            }
        }

        ssize_t n = read(ch->sock, raw, sizeof(raw) - 1);
        if (n <= 0) {
            if (n < 0) LOG_WARN("WhatsApp: read error: %s", strerror(errno));
            else       LOG_WARN("WhatsApp: bridge closed connection");
            close(ch->sock); ch->sock = -1;
            sleep(3);
            continue;
        }
        raw[n] = '\0';

        /* Accumulate into line buffer, process on each '\n' */
        for (ssize_t i = 0; i < n; i++) {
            if (raw[i] == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) {
                    cJSON *obj = cJSON_Parse(line);
                    if (obj) {
                        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "type"));
                        if (type) {
                            if (strcmp(type, "message") == 0) handle_message(ch, obj);
                            else if (strcmp(type, "status") == 0) {
                                const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "status"));
                                if (s) handle_status(s);
                            } else if (strcmp(type, "error") == 0) {
                                const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "error"));
                                LOG_WARN("WhatsApp bridge error: %s", e ? e : "unknown");
                            }
                        }
                        cJSON_Delete(obj);
                    } else {
                        LOG_WARN("WhatsApp: bad JSON from bridge: %.80s", line);
                    }
                }
                line_len = 0;
            } else if (line_len < LINE_MAX - 1) {
                line[line_len++] = raw[i];
            }
        }
    }

    if (ch->sock >= 0) { close(ch->sock); ch->sock = -1; }
    return NULL;
}

/* ── Dispatcher send callback ───────────────────────────────────── */

/* Called by the outbound dispatcher when the agent produces a reply
 * on channel="whatsapp". Extracts the phone number from chat_id
 * (format "whatsapp:<pn>") and sends via the bridge. */
static void whatsapp_dispatch_send(channel_t *self, outbound_msg_t *msg) {
    whatsapp_channel_t *ch = (whatsapp_channel_t *)self->priv;

    /* Extract phone number from chat_id "whatsapp:<pn>" */
    const char *pn = msg->chat_id;
    const char *colon = strchr(pn, ':');
    if (colon) pn = colon + 1;

    if (!pn || !*pn) {
        LOG_WARN("WhatsApp dispatch: cannot extract phone from chat_id '%s'", msg->chat_id);
        return;
    }

    /* Extract replyJid from metadata "replyJid=<jid>" if present.
     * This is the original @lid JID that Baileys must use for LID-routed replies. */
    char reply_jid[256] = "";
    if (msg->metadata) {
        const char *tag = strstr(msg->metadata, "replyJid=");
        if (tag) {
            tag += 9; /* skip "replyJid=" */
            const char *end = strchr(tag, ';'); /* in case multiple k=v pairs */
            size_t len = end ? (size_t)(end - tag) : strlen(tag);
            if (len < sizeof(reply_jid))
                memcpy(reply_jid, tag, len);
        }
    }

    LOG_DEBUG("WhatsApp dispatch: reply to pn=%s replyJid=%s",
              pn, reply_jid[0] ? reply_jid : "(none)");

    if (!ch || ch->sock < 0) {
        LOG_WARN("WhatsApp dispatch: bridge not connected, dropping reply");
        return;
    }

    char jid[256];
    ensure_jid(pn, jid, sizeof(jid));  /* fallback phone JID */

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "send");
    cJSON_AddStringToObject(obj, "to",   jid);
    if (reply_jid[0])
        cJSON_AddStringToObject(obj, "replyJid", reply_jid);
    cJSON_AddStringToObject(obj, "text", msg->content);
    char *js = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!js) return;

    send_line(ch, js);
    free(js);
}

/* ── Public API ─────────────────────────────────────────────────── */

whatsapp_channel_t *whatsapp_channel_create(config_t *cfg, bus_t *bus, allowlist_t *al) {
    const char *host = config_get(cfg, "whatsapp", "bridge_host", NULL);
    if (!host || !*host) {
        LOG_DEBUG("WhatsApp: no [whatsapp] bridge_host configured — channel disabled");
        return NULL;
    }

    whatsapp_channel_t *ch = calloc(1, sizeof(*ch));
    snprintf(ch->host,  sizeof(ch->host),  "%s", host);
    ch->port = config_get_int(cfg, "whatsapp", "bridge_port", 3001);
    const char *tok = config_get(cfg, "whatsapp", "bridge_token", "");
    if (tok) snprintf(ch->token, sizeof(ch->token), "%s", tok);
    ch->bus      = bus;
    ch->allowlist = al;
    ch->sock     = -1;
    ch->running  = false;
    pthread_mutex_init(&ch->send_lock, NULL);

    /* Register in channel registry so dispatcher routes replies here */
    strncpy(ch->channel_iface.name, "whatsapp", CHANNEL_NAME_MAX - 1);
    ch->channel_iface.send = whatsapp_dispatch_send;
    ch->channel_iface.priv = ch;
    channel_register(&ch->channel_iface);

    LOG_DEBUG("WhatsApp channel created (%s:%d)", ch->host, ch->port);
    return ch;
}

bool whatsapp_channel_start(whatsapp_channel_t *ch) {
    if (!ch) return false;
    ch->running = true;
    if (pthread_create(&ch->thread, NULL, recv_thread, ch) != 0) {
        LOG_WARN("WhatsApp: failed to start receive thread");
        ch->running = false;
        return false;
    }
    LOG_DEBUG("WhatsApp channel started");
    return true;
}

void whatsapp_channel_destroy(whatsapp_channel_t *ch) {
    if (!ch) return;
    ch->running = false;
    if (ch->sock >= 0) { shutdown(ch->sock, SHUT_RDWR); close(ch->sock); ch->sock = -1; }
    pthread_join(ch->thread, NULL);
    pthread_mutex_destroy(&ch->send_lock);
    free(ch);
}

bool whatsapp_channel_send(whatsapp_channel_t *ch, const char *to, const char *text) {
    if (!ch || !to || !text) return false;

    char jid[128];
    ensure_jid(to, jid, sizeof(jid));

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "send");
    cJSON_AddStringToObject(obj, "to",   jid);
    cJSON_AddStringToObject(obj, "text", text);
    char *js = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!js) return false;

    bool ok = send_line(ch, js);
    free(js);
    return ok;
}

bool whatsapp_channel_is_allowed(whatsapp_channel_t *ch, const char *pn) {
    if (!ch || !ch->allowlist) return true;
    return allowlist_check(ch->allowlist, ACL_EMAIL, pn);
}
