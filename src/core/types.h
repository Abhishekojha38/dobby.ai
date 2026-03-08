/*
 * types.h — Core types for Dobby AI Agent
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result type */

typedef enum {
    OK = 0,
    ERR_GENERIC,
    ERR_ALLOC,
    ERR_IO,
    ERR_PARSE,
    ERR_NETWORK,
    ERR_NOT_FOUND,
    ERR_PERMISSION,
    ERR_TIMEOUT,
    ERR_MAX_ITERATIONS,
} status_t;

typedef struct {
    status_t status;
    char          *message;   /* Heap-allocated, caller frees. NULL on OK. */
} result_t;

result_t ok(void);
result_t err(status_t status, const char *fmt, ...);
void           result_free(result_t *result);

/* String buffer */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} strbuf_t;

void strbuf_init(strbuf_t *sb, size_t initial_cap);
void strbuf_append(strbuf_t *sb, const char *str);
void strbuf_appendf(strbuf_t *sb, const char *fmt, ...);
void strbuf_append_len(strbuf_t *sb, const char *str, size_t len);
void strbuf_clear(strbuf_t *sb);
void strbuf_free(strbuf_t *sb);

/* String helpers */

char *strdup(const char *s);
char *strndup(const char *s, size_t n);
bool  str_starts_with(const char *s, const char *prefix);
bool  str_ends_with(const char *s, const char *suffix);
char *str_trim(char *s);

/*
 * sanitize_output — strip ANSI escape codes and non-printable control
 * characters from tool output before it enters the LLM message history.
 *
 * Raw terminal output from embedded tools (dmesg, top, serial) contains
 * ESC sequences, \r, and other control chars that cause JSON parse failures
 * in some upstream providers when cJSON encodes them as \uXXXX escapes.
 *
 * Keeps: printable ASCII (0x20–0x7e), \n (0x0a), \t (0x09)
 * Strips: ESC[...] ANSI sequences, bare \r, other control characters
 *
 * Returns a newly heap-allocated string.  Caller must free().
 */
char *sanitize_output(const char *src);

/* Forward declarations */

typedef struct context context_t;

#ifdef __cplusplus
}
#endif

#endif /* TYPES_H */
