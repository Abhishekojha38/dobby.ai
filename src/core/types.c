/*
 * types.c — Core types implementation
 */
#include "types.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Result */

result_t ok(void) {
    return (result_t){ .status = OK, .message = NULL };
}

result_t err(status_t status, const char *fmt, ...) {
    result_t r;
    r.status = status;

    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    r.message = strdup(buf);
    return r;
}

void result_free(result_t *result) {
    if (result && result->message) {
        free(result->message);
        result->message = NULL;
    }
}

/* String buffer */

void strbuf_init(strbuf_t *sb, size_t initial_cap) {
    sb->cap  = initial_cap > 0 ? initial_cap : 256;
    sb->data = malloc(sb->cap);
    if (!sb->data) {
        /* Fatal: callers assume strbuf is always valid. Abort rather than
         * silently corrupt — OOM at this level means the process is doomed. */
        fprintf(stderr, "dobby: strbuf_init: out of memory (%zu bytes)\n", sb->cap);
        abort();
    }
    sb->data[0] = '\0';
    sb->len  = 0;
}

void strbuf_append(strbuf_t *sb, const char *str) {
    size_t slen = strlen(str);
    while (sb->len + slen + 1 > sb->cap) {
        sb->cap *= 2;
        char *tmp = realloc(sb->data, sb->cap);
        if (!tmp) {
            fprintf(stderr, "dobby: strbuf_append: out of memory (%zu bytes)\n", sb->cap);
            abort();
        }
        sb->data = tmp;
    }
    memcpy(sb->data + sb->len, str, slen + 1);
    sb->len += slen;
}

void strbuf_appendf(strbuf_t *sb, const char *fmt, ...) {
    /* Two-pass: measure then write — avoids 4 KB stack limit and silent truncation. */
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);

    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (needed < 0) { va_end(args2); return; } /* format error */

    char *buf = malloc((size_t)needed + 1);
    if (!buf) {
        va_end(args2);
        fprintf(stderr, "dobby: strbuf_appendf: out of memory\n");
        abort();
    }
    vsnprintf(buf, (size_t)needed + 1, fmt, args2);
    va_end(args2);

    strbuf_append(sb, buf);
    free(buf);
}

void strbuf_clear(strbuf_t *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

void strbuf_free(strbuf_t *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

/* String helpers */

char *strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup  = malloc(len + 1);
    if (dup) memcpy(dup, s, len + 1);
    return dup;
}

char *strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (n < len) len = n;
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}

bool str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool str_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t slen = strlen(s);
    size_t plen = strlen(suffix);
    if (plen > slen) return false;
    return strcmp(s + slen - plen, suffix) == 0;
}

char *str_trim(char *s) {
    if (!s) return NULL;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    return s;
}

void strbuf_append_len(strbuf_t *sb, const char *str, size_t len) {
    if (!str || len == 0) return;
    while (sb->len + len + 1 > sb->cap) {
        sb->cap *= 2;
        char *tmp = realloc(sb->data, sb->cap);
        if (!tmp) {
            fprintf(stderr, "dobby: strbuf_append_len: out of memory (%zu bytes)\n", sb->cap);
            abort();
        }
        sb->data = tmp;
    }
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

/* Output sanitization */

char *sanitize_output(const char *src) {
    if (!src) return strdup("");

    size_t len = strlen(src);
    char  *dst = malloc(len + 1);
    if (!dst) return strdup(src);  /* OOM fallback */

    const unsigned char *s = (const unsigned char *)src;
    char *d = dst;

    while (*s) {
        /* ANSI/VT escape sequence: ESC [ ... <final 0x40-0x7e>
         *                      or: ESC <single char 0x40-0x5f>      */
        if (*s == 0x1b) {
            s++;  /* skip ESC */
            if (*s == '[') {
                /* CSI sequence — skip parameter/intermediate bytes,
                 * then skip the single final byte (0x40–0x7e). */
                s++;
                while (*s && (*s < 0x40 || *s > 0x7e)) s++;
                if (*s) s++;
            } else if (*s >= 0x40 && *s <= 0x5f) {
                s++;  /* two-byte Fe escape */
            }
            /* bare ESC with no recognised follower: just skip the ESC */
            continue;
        }

        /* \r: keep only when followed by \n (Windows CRLF),
         * discard otherwise (terminal carriage-return overwrite). */
        if (*s == '\r') {
            if (*(s + 1) == '\n') s++;  /* will emit \n next iteration */
            else                  s++;  /* bare \r — discard */
            continue;
        }

        /* Always keep \n and \t */
        if (*s == '\n' || *s == '\t') { *d++ = (char)*s++; continue; }

        /* Printable ASCII 0x20 – 0x7e */
        if (*s >= 0x20 && *s <= 0x7e) { *d++ = (char)*s++; continue; }

        /* Everything else (other control chars, high bytes) — discard */
        s++;
    }

    *d = '\0';
    return dst;
}
