/*
 * test_email_parsing.c — Unit tests for email_channel parsing logic
 *
 * Tests all parsing functions in isolation without needing a real
 * IMAP/SMTP server or libcurl.
 *
 * Build:
 *   gcc -std=c17 -D_GNU_SOURCE -Wall -Wextra \
 *       -I../../src -I../../src/channels/email \
 *       test_email_parsing.c -o test_email_parsing
 * Run:
 *   ./test_email_parsing
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <assert.h>

/* Forward declarations for test helpers defined later in file */
static const char *strip_fetch_envelope(const char *raw);
static char *extract_body_v2(const char *msg);


/* ── Minimal stubs so we can compile the parsing functions stand-alone ── */

#define EMAIL_ADDR_MAX   256
#define EMAIL_BODY_MAX   16384
#define EMAIL_SUBJ_MAX   512

/* Copy only the three parsing functions from email_channel.c inline.
 * This avoids linking against libcurl or any other dependency. */

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
            size_t len = (size_t)(end - v);
            while (len > 0 && (v[len-1] == '\r' || v[len-1] == ' ')) len--;
            return strndup(v, len);
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return NULL;
}

static char *extract_body(const char *msg) {
    if (!msg) return NULL;
    const char *body = strstr(msg, "\r\n\r\n");
    if (body) body += 4;
    else {
        body = strstr(msg, "\n\n");
        if (body) body += 2;
        else return strdup(msg);
    }
    size_t len = strlen(body);
    while (len > 0 && (body[len-1] == '\r' || body[len-1] == '\n' ||
                        body[len-1] == ' ')) len--;
    if (len > EMAIL_BODY_MAX) len = EMAIL_BODY_MAX;
    return strndup(body, len);
}

static char *extract_addr(const char *from) {
    if (!from) return NULL;
    const char *lt = strchr(from, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt && gt > lt + 1)
        return strndup(lt + 1, (size_t)(gt - lt - 1));
    char *copy = strdup(from);
    for (char *p = copy + strlen(copy) - 1; p >= copy && isspace((unsigned char)*p); p--)
        *p = '\0';
    return copy;
}

/* UID parsing logic extracted from poll_fn for isolated testing */
static int parse_uid_list(const char *result, unsigned long *uids, int max_uids) {
    if (!result || !*result) return 0;
    char *copy = strdup(result);
    char *p = copy;
    if (strncasecmp(p, "* SEARCH", 8) == 0) p += 8;
    while (*p == ' ') p++;

    int count = 0;
    char *tok = strtok(p, " \r\n");
    while (tok && count < max_uids) {
        unsigned long uid = strtoul(tok, NULL, 10);
        if (uid > 0) uids[count++] = uid;
        tok = strtok(NULL, " \r\n");
    }
    free(copy);
    return count;
}

/* ── Test framework ─────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define TEST(name) do { \
    printf("  %-55s", name); fflush(stdout); \
} while(0)

#define PASS() do { printf("\033[32mPASS\033[0m\n"); g_pass++; } while(0)
#define FAIL(msg) do { \
    printf("\033[31mFAIL\033[0m  %s\n", msg); g_fail++; \
} while(0)

#define EXPECT_STR(got, want) do { \
    if ((got) && (want) && strcmp((got),(want)) == 0) PASS(); \
    else FAIL("got='" #got "' want='" want "'"); \
} while(0)

#define EXPECT_NULL(got) do { \
    if ((got) == NULL) PASS(); \
    else FAIL("expected NULL, got non-NULL"); \
} while(0)

#define EXPECT_INT(got, want) do { \
    if ((got) == (want)) PASS(); \
    else { char _m[128]; snprintf(_m,128,"got=%d want=%d",(int)(got),(int)(want)); FAIL(_m); } \
} while(0)

/* ── Tests: extract_addr ────────────────────────────────────────── */

static void test_extract_addr(void) {
    printf("\nextract_addr()\n");
    char *r;

    TEST("bare address");
    r = extract_addr("user@example.com");
    EXPECT_STR(r, "user@example.com"); free(r);

    TEST("name + angle brackets");
    r = extract_addr("Alice Smith <alice@example.com>");
    EXPECT_STR(r, "alice@example.com"); free(r);

    TEST("no name, just angle brackets");
    r = extract_addr("<bob@example.com>");
    EXPECT_STR(r, "bob@example.com"); free(r);

    TEST("trailing whitespace stripped");
    r = extract_addr("user@example.com   ");
    EXPECT_STR(r, "user@example.com"); free(r);

    TEST("name with dots and plus");
    r = extract_addr("Jane Doe <jane.doe+tag@mail.example.org>");
    EXPECT_STR(r, "jane.doe+tag@mail.example.org"); free(r);

    TEST("NULL input returns NULL");
    r = extract_addr(NULL);
    EXPECT_NULL(r);

    TEST("empty string returns empty");
    r = extract_addr("");
    /* empty string: not NULL but empty */
    if (r && strcmp(r, "") == 0) PASS(); else FAIL("expected empty string");
    free(r);

    TEST("quoted display name");
    r = extract_addr("\"Dobby Bot\" <dobby@example.com>");
    EXPECT_STR(r, "dobby@example.com"); free(r);
}

/* ── Tests: extract_header ──────────────────────────────────────── */

static void test_extract_header(void) {
    printf("\nextract_header()\n");
    char *r;

    const char *simple =
        "From: Alice <alice@example.com>\r\n"
        "To: bob@example.com\r\n"
        "Subject: Hello World\r\n"
        "Date: Mon, 1 Jan 2024 12:00:00 +0000\r\n"
        "\r\n"
        "Body text here.\r\n";

    TEST("From: header found");
    r = extract_header(simple, "From");
    EXPECT_STR(r, "Alice <alice@example.com>"); free(r);

    TEST("Subject: header found");
    r = extract_header(simple, "Subject");
    EXPECT_STR(r, "Hello World"); free(r);

    TEST("To: header found");
    r = extract_header(simple, "To");
    EXPECT_STR(r, "bob@example.com"); free(r);

    TEST("case-insensitive match");
    r = extract_header(simple, "SUBJECT");
    EXPECT_STR(r, "Hello World"); free(r);

    TEST("missing header returns NULL");
    r = extract_header(simple, "X-Custom");
    EXPECT_NULL(r);

    TEST("NULL message returns NULL");
    r = extract_header(NULL, "From");
    EXPECT_NULL(r);

    TEST("NULL field returns NULL");
    r = extract_header(simple, NULL);
    EXPECT_NULL(r);

    /* LF-only line endings */
    const char *lf_only =
        "From: carol@example.com\n"
        "Subject: LF test\n"
        "\n"
        "body\n";

    TEST("LF-only line endings");
    r = extract_header(lf_only, "Subject");
    EXPECT_STR(r, "LF test"); free(r);

    /* Header with extra spaces after colon */
    const char *spaced =
        "From:    spaced@example.com\r\n"
        "\r\n";

    TEST("extra spaces after colon trimmed");
    r = extract_header(spaced, "From");
    EXPECT_STR(r, "spaced@example.com"); free(r);

    /* Partial match must not fire — "Fromage" must not match "From" */
    const char *partial =
        "Fromage: cheese@dairy.com\r\n"
        "From: real@example.com\r\n"
        "\r\n";

    TEST("partial field name not matched (Fromage != From)");
    r = extract_header(partial, "From");
    EXPECT_STR(r, "real@example.com"); free(r);
}

/* ── Tests: extract_body ────────────────────────────────────────── */

static void test_extract_body(void) {
    printf("\nextract_body()\n");
    char *r;

    TEST("CRLF blank line separator");
    r = extract_body("From: x@y.com\r\nSubject: hi\r\n\r\nHello body.");
    EXPECT_STR(r, "Hello body."); free(r);

    TEST("LF blank line separator");
    r = extract_body("From: x@y.com\nSubject: hi\n\nHello body.");
    EXPECT_STR(r, "Hello body."); free(r);

    TEST("trailing newlines stripped");
    r = extract_body("Headers\r\n\r\nBody text\r\n\r\n");
    EXPECT_STR(r, "Body text"); free(r);

    TEST("multi-line body preserved");
    r = extract_body("Subject: test\r\n\r\nLine 1\r\nLine 2\r\nLine 3");
    EXPECT_STR(r, "Line 1\r\nLine 2\r\nLine 3"); free(r);

    TEST("empty body returns empty string");
    r = extract_body("Headers\r\n\r\n");
    if (r && strcmp(r, "") == 0) PASS(); else FAIL("expected empty string");
    free(r);

    TEST("NULL returns NULL");
    r = extract_body(NULL);
    EXPECT_NULL(r);

    TEST("no blank line: whole message returned as body");
    r = extract_body("no blank line at all");
    EXPECT_STR(r, "no blank line at all"); free(r);

    TEST("body with Unicode preserved");
    r = extract_body("Subject: x\r\n\r\n你好 Dobby ⚡");
    EXPECT_STR(r, "你好 Dobby ⚡"); free(r);
}

/* ── Tests: IMAP SEARCH response UID parsing ────────────────────── */

static void test_uid_parsing(void) {
    printf("\nUID list parsing (from poll_fn logic)\n");

    unsigned long uids[64];
    int n;

    TEST("standard IMAP SEARCH response");
    n = parse_uid_list("* SEARCH 1 2 3\r\n", uids, 64);
    EXPECT_INT(n, 3);
    if (uids[0]==1 && uids[1]==2 && uids[2]==3) PASS(); else FAIL("wrong uid values");

    TEST("bare UID list (no * SEARCH prefix)");
    n = parse_uid_list("10 20 30", uids, 64);
    EXPECT_INT(n, 3);

    TEST("single UID");
    n = parse_uid_list("* SEARCH 42\r\n", uids, 64);
    EXPECT_INT(n, 1);
    EXPECT_INT((int)uids[0], 42);

    TEST("empty SEARCH result (no unseen messages)");
    n = parse_uid_list("* SEARCH \r\n", uids, 64);
    EXPECT_INT(n, 0);

    TEST("NULL result");
    n = parse_uid_list(NULL, uids, 64);
    EXPECT_INT(n, 0);

    TEST("empty string");
    n = parse_uid_list("", uids, 64);
    EXPECT_INT(n, 0);

    TEST("case-insensitive * search prefix");
    n = parse_uid_list("* search 5 6", uids, 64);
    EXPECT_INT(n, 2);

    TEST("large UIDs (real world: > 100000)");
    n = parse_uid_list("* SEARCH 100001 200002 300003", uids, 64);
    EXPECT_INT(n, 3);
    if (uids[0]==100001 && uids[1]==200002 && uids[2]==300003) PASS();
    else FAIL("large UIDs parsed incorrectly");
}

/* ── Tests: end-to-end message parsing (realistic RFC 2822) ─────── */

static void test_real_email(void) {
    printf("\nReal-world RFC 2822 messages\n");

    /* A realistic Gmail-style message */
    const char *gmail =
        "Delivered-To: dobby@example.com\r\n"
        "Received: by 2002:a05:6a00:1a4b:0:0:0:0 with SMTP id ...\r\n"
        "From: John User <john.user@gmail.com>\r\n"
        "To: dobby@example.com\r\n"
        "Subject: Can you check disk space?\r\n"
        "Date: Mon, 4 Mar 2024 09:15:33 +0000\r\n"
        "Message-ID: <CABcdef123@mail.gmail.com>\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
        "\r\n"
        "Hi Dobby,\r\n"
        "\r\n"
        "Can you check how much disk space is left on the server?\r\n"
        "\r\n"
        "Thanks\r\n";

    char *from_raw = extract_header(gmail, "From");
    char *from     = from_raw ? extract_addr(from_raw) : NULL;
    char *subject  = extract_header(gmail, "Subject");
    char *body     = extract_body(gmail);

    TEST("Gmail: From address extracted");
    EXPECT_STR(from, "john.user@gmail.com");

    TEST("Gmail: Subject extracted");
    EXPECT_STR(subject, "Can you check disk space?");

    TEST("Gmail: Body starts correctly");
    if (body && strncmp(body, "Hi Dobby,", 9) == 0) PASS();
    else FAIL("body start mismatch");

    TEST("Gmail: Body doesn't include headers");
    if (body && strstr(body, "Content-Type:") == NULL) PASS();
    else FAIL("headers leaked into body");

    free(from_raw); free(from); free(subject); free(body);

    /* Reply-chain email with Re: subject */
    const char *reply =
        "From: support@company.com\r\n"
        "Subject: Re: [Dobby] check disk space\r\n"
        "\r\n"
        "Done, 45GB free.\r\n";

    char *subj2 = extract_header(reply, "Subject");
    TEST("Reply subject with Re: prefix");
    EXPECT_STR(subj2, "Re: [Dobby] check disk space");
    free(subj2);
}

/* ── Main ───────────────────────────────────────────────────────── */

static void test_fetch_envelope_stripping(void) {
    printf("\nFETCH envelope stripping\n");

    /* Standard Gmail UID FETCH response */
    const char *gmail_fetch =
        "* 5 FETCH (RFC822 {312}\r\n"
        "From: alice@gmail.com\r\n"
        "To: dobby@test.com\r\n"
        "Subject: Test\r\n"
        "\r\n"
        "Hello Dobby!\r\n"
        ")\r\n";

    const char *msg = strip_fetch_envelope(gmail_fetch);

    TEST("FETCH envelope: From header found after strip");
    char *from = extract_header(msg, "From");
    EXPECT_STR(from, "alice@gmail.com"); free(from);

    TEST("FETCH envelope: Subject found after strip");
    char *subj = extract_header(msg, "Subject");
    EXPECT_STR(subj, "Test"); free(subj);

    TEST("FETCH envelope: body extracted after strip");
    char *body = extract_body(msg);
    if (body && strncmp(body, "Hello Dobby!", 12) == 0) PASS();
    else FAIL("body mismatch");
    free(body);

    TEST("FETCH envelope: no envelope = passthrough");
    const char *plain = "From: bob@test.com\r\n\r\nHi\r\n";
    const char *passthrough = strip_fetch_envelope(plain);
    if (passthrough == plain) PASS(); else FAIL("should be unchanged");

    /* No {size} literal marker — just a bare FETCH line */
    const char *bare_fetch =
        "* 2 FETCH (FLAGS (\\Seen))\n"
        "From: carol@test.com\r\n"
        "\r\n"
        "body\r\n";

    TEST("FETCH envelope: bare fetch line (no {size}) skipped");
    const char *msg2 = strip_fetch_envelope(bare_fetch);
    char *from2 = extract_header(msg2, "From");
    EXPECT_STR(from2, "carol@test.com"); free(from2);
}

static size_t rtrim(const char *s, size_t len) {
    while (len > 0 && (s[len-1] == '\r' || s[len-1] == '\n' || s[len-1] == ' '))
        len--;
    return len;
}

static char *extract_body_v2(const char *msg) {
    if (!msg) return NULL;

    char *ct = extract_header(msg, "Content-Type");
    bool is_multipart = ct && strstr(ct, "multipart");

    if (is_multipart) {
        const char *bp = strstr(ct, "boundary");
        char boundary[256] = {0};
        if (bp) {
            bp += 8;
            while (*bp == ' ' || *bp == '=') bp++;
            bool quoted = (*bp == '"');
            if (quoted) bp++;
            size_t bi = 0;
            while (*bp && bi < sizeof(boundary)-3) {
                if (quoted && *bp == '"') break;
                if (!quoted && (*bp == ';' || *bp == '\r' || *bp == '\n')) break;
                boundary[bi++] = *bp++;
            }
            memmove(boundary + 2, boundary, bi + 1);
            boundary[0] = '-'; boundary[1] = '-';
        }
        free(ct);

        if (!boundary[2]) goto simple;

        const char *p = msg;
        while ((p = strstr(p, boundary)) != NULL) {
            p += strlen(boundary);
            if (*p == '\r') p++;
            if (*p == '\n') p++;
            if (*p == '-') break;

            const char *part_start = p;
            char *part_ct = extract_header(part_start, "Content-Type");
            bool is_plain = part_ct && strncasecmp(part_ct, "text/plain", 10) == 0;
            free(part_ct);

            const char *part_body = strstr(part_start, "\r\n\r\n");
            if (!part_body) part_body = strstr(part_start, "\n\n");
            if (part_body) {
                part_body += (part_body[0] == '\r') ? 4 : 2;
            } else continue;

            if (is_plain) {
                const char *end = strstr(part_body, boundary);
                size_t len = end ? (size_t)(end - part_body) : strlen(part_body);
                len = rtrim(part_body, len);
                if (len > 16384) len = 16384;
                return strndup(part_body, len);
            }
        }
        return strdup("(no plain text body)");
    }
    free(ct);

simple:;
    const char *body = strstr(msg, "\r\n\r\n");
    if (body) body += 4;
    else {
        body = strstr(msg, "\n\n");
        if (body) body += 2;
        else return strdup(msg);
    }
    size_t len = rtrim(body, strlen(body));
    if (len > 16384) len = 16384;
    return strndup(body, len);
}

static void test_multipart_body(void) {
    printf("\nMultipart body extraction\n");
    char *r;

    /* Realistic Gmail multipart/alternative email */
    const char *gmail_multipart =
        "From: alice@gmail.com\r\n"
        "To: dobby@test.com\r\n"
        "Subject: Check disk space\r\n"
        "Content-Type: multipart/alternative; boundary=\"000000000000abc123\"\r\n"
        "\r\n"
        "--000000000000abc123\r\n"
        "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
        "\r\n"
        "How much disk space is free?\r\n"
        "\r\n"
        "--000000000000abc123\r\n"
        "Content-Type: text/html; charset=\"UTF-8\"\r\n"
        "\r\n"
        "<div>How much disk space is free?</div>\r\n"
        "\r\n"
        "--000000000000abc123--\r\n";

    TEST("Gmail multipart: text/plain extracted");
    r = extract_body_v2(gmail_multipart);
    EXPECT_STR(r, "How much disk space is free?"); free(r);

    TEST("Gmail multipart: not HTML content");
    r = extract_body_v2(gmail_multipart);
    if (r && strstr(r, "<div>") == NULL) PASS(); else FAIL("HTML leaked in");
    free(r);

    TEST("Gmail multipart: not MIME boundary");
    r = extract_body_v2(gmail_multipart);
    if (r && strstr(r, "abc123") == NULL) PASS(); else FAIL("boundary leaked in");
    free(r);

    /* Simple plain text (no multipart) */
    const char *plain =
        "From: bob@test.com\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello Dobby!";

    TEST("Simple text/plain passthrough");
    r = extract_body_v2(plain);
    EXPECT_STR(r, "Hello Dobby!"); free(r);

    /* Multipart with plain first */
    const char *plain_first =
        "Content-Type: multipart/mixed; boundary=\"XYZ\"\r\n"
        "\r\n"
        "--XYZ\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Plain text part\r\n"
        "--XYZ\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "(attachment bytes)\r\n"
        "--XYZ--\r\n";

    TEST("Multipart/mixed: plain part extracted, attachment ignored");
    r = extract_body_v2(plain_first);
    EXPECT_STR(r, "Plain text part"); free(r);

    /* NULL */
    TEST("NULL returns NULL");
    r = extract_body_v2(NULL);
    EXPECT_NULL(r);
}
int main(void) {
    printf("━━━ Dobby Email Channel — Unit Tests ━━━━━━━━━━━━━━━━━━━━━━━\n");

    test_extract_addr();
    test_extract_header();
    test_extract_body();
    test_uid_parsing();
    test_real_email();
    test_fetch_envelope_stripping();
    test_multipart_body();

    printf("\n━━━ Results: \033[32m%d passed\033[0m", g_pass);
    if (g_fail > 0) printf(", \033[31m%d FAILED\033[0m", g_fail);
    printf(" ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    return g_fail > 0 ? 1 : 0;
}

/* ── Tests: IMAP FETCH envelope stripping ───────────────────────── */

/* Replicate the envelope-stripping logic from process_uid() */
static const char *strip_fetch_envelope(const char *raw) {
    if (strncmp(raw, "* ", 2) == 0) {
        const char *brace = strchr(raw, '{');
        if (brace) {
            const char *nl = strchr(brace, '\n');
            if (nl) return nl + 1;
        } else {
            const char *nl = strchr(raw, '\n');
            if (nl) return nl + 1;
        }
    }
    return raw;
}


/* ── Tests: multipart body extraction ──────────────────────────── */

/* Paste rtrim + extract_body from email_channel.c for standalone testing */


