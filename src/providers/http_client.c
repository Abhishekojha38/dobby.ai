/*
 * http_client.c — HTTP client using libcurl (required dependency).
 *
 * libcurl is the only supported HTTP backend.  The previous socket-based
 * fallback has been removed:
 *   - It lacked TLS support entirely (no HTTPS).
 *   - Its HTTP/1.1 parser was incomplete (chunked transfer encoding,
 *     redirects, keep-alive all unhandled).
 *   - Maintenance burden outweighed any benefit — libcurl is available on
 *     every Linux embedded platform (uclibc, musl, OpenWRT, Yocto, Buildroot).
 *
 * Embedded notes:
 *   - CURLOPT_BUFFERSIZE is set to a small value to keep curl's internal
 *     receive buffer lean on low-RAM targets.
 *   - CURLOPT_NOSIGNAL=1 prevents curl from touching SIGALRM, which is
 *     critical because other subsystems (scheduler) may be sensitive to
 *     signal interference.
 *   - The write callback doubles its buffer capacity rather than pre-
 *     allocating; this keeps peak RSS proportional to actual response size.
 */
#include "http_client.h"
#include "../core/log.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

/* Write buffer */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} write_buf_t;

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t       total = size * nmemb;
    write_buf_t *buf   = (write_buf_t *)userp;

    /* Grow buffer if needed */
    size_t new_cap = buf->cap > 0 ? buf->cap : 4096;
    while (buf->len + total + 1 > new_cap) {
        new_cap *= 2;
    }
    if (new_cap != buf->cap) {
        char *tmp = realloc(buf->data, new_cap);
        if (!tmp) return 0; /* Tell curl to abort */
        buf->data = tmp;
        buf->cap  = new_cap;
    }

    memcpy(buf->data + buf->len, ptr, total);
    buf->len            += total;
    buf->data[buf->len]  = '\0';
    return total;
}

/* Lifecycle */

void http_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    LOG_DEBUG("HTTP client initialised (libcurl %s)", curl_version());
}

void http_cleanup(void) {
    curl_global_cleanup();
}

/* Shared curl setup */

static CURL *make_curl_handle(const char *url, write_buf_t *buf,
                              long timeout_seconds) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_URL,          url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     timeout_seconds > 0 ? timeout_seconds : 120L);
    /* Prevent curl from sending SIGALRM — essential in multi-threaded code */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    /* Small internal buffer — reduces RSS on embedded targets */
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);
    /* Follow redirects */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,       5L);

    return curl;
}

/* Public API */

http_response_t http_post_json(const char *url,
                                           const char *json_body,
                                           long        timeout_seconds) {
    http_response_t resp = {0};

    write_buf_t buf = { .data = malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.data) {
        resp.error = strdup("Out of memory");
        return resp;
    }
    buf.data[0] = '\0';

    CURL *curl = make_curl_handle(url, &buf, timeout_seconds);
    if (!curl) {
        free(buf.data);
        resp.error = strdup("Failed to initialise curl handle");
        return resp;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        LOG_ERROR("HTTP POST failed: %s", curl_easy_strerror(res));
        resp.error = strdup(curl_easy_strerror(res));
        free(buf.data);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status_code);
        resp.body     = buf.data;
        resp.body_len = buf.len;
    }

    curl_easy_cleanup(curl);
    return resp;
}

http_response_t http_post_json_auth_ex(const char  *url,
                                                    const char  *json_body,
                                                    const char  *auth_header,
                                                    const char * const *extra_headers,
                                                    long         timeout_seconds) {
    http_response_t resp = {0};

    write_buf_t buf = { .data = malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.data) { resp.error = strdup("Out of memory"); return resp; }
    buf.data[0] = '\0';

    CURL *curl = make_curl_handle(url, &buf, timeout_seconds);
    if (!curl) {
        free(buf.data);
        resp.error = strdup("Failed to initialise curl handle");
        return resp;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (auth_header && *auth_header) {
        char hdr[512];
        snprintf(hdr, sizeof(hdr), "Authorization: %s", auth_header);
        headers = curl_slist_append(headers, hdr);
    }

    if (extra_headers) {
        for (int i = 0; extra_headers[i]; i++)
            headers = curl_slist_append(headers, extra_headers[i]);
    }

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        LOG_ERROR("HTTP POST failed: %s", curl_easy_strerror(res));
        resp.error = strdup(curl_easy_strerror(res));
        free(buf.data);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status_code);
        resp.body     = buf.data;
        resp.body_len = buf.len;
    }

    curl_easy_cleanup(curl);
    return resp;
}


void http_response_free(http_response_t *resp) {
    free(resp->body);
    free(resp->error);
    memset(resp, 0, sizeof(*resp));
}
