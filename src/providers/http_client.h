/*
 * http_client.h — Simple HTTP client wrapper (libcurl)
 */
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "../core/dobby.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    status_code;
    char  *body;
    size_t body_len;
    char  *error;
} http_response_t;

/* Initialize HTTP subsystem (call once). */
void http_init(void);

/* Cleanup HTTP subsystem. */
void http_cleanup(void);

/* POST JSON to a URL. Returns response. Caller must free with http_response_free. */
http_response_t http_post_json(const char *url,
                                            const char *json_body,
                                            long timeout_seconds);

/* POST JSON with Authorization header plus arbitrary extra headers.
 * extra_headers: NULL-terminated array of "Name: Value" strings, or NULL.
 * Example: const char *hdrs[] = {"HTTP-Referer: https://x", "X-Title: App", NULL};
 */
http_response_t http_post_json_auth_ex(const char  *url,
                                                    const char  *json_body,
                                                    const char  *auth_header,
                                                    const char * const *extra_headers,
                                                    long         timeout_seconds);

/* Free response. */
void http_response_free(http_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_H */
