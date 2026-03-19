/*
 * gateway.c — HTTP gateway: REST API + web chat UI
 *
 * Phase 2 change: handle_chat() no longer calls agent_chat() directly.
 * Instead it posts an inbound_msg_t to the bus and blocks on a
 * response_pair_t until the agent worker delivers the reply.
 * The agent_mutex is gone — serialisation is the bus's responsibility.
 */
#define _GNU_SOURCE
#include "gateway.h"
#include "../core/log.h"
#include "../core/cJSON.h"
#include "../tools/scheduler/scheduler.h"
#include "../tools/subagent/subagent.h"
#include "../channels/email/email_channel.h"
#include "../security/allowlist.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define GATEWAY_MAX_THREADS    8
#define GATEWAY_BODY_LIMIT     (32 * 1024)
#define GATEWAY_RECV_BUF       (64 * 1024)
#define GATEWAY_BACKLOG        16

static const char FALLBACK_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<title>Dobby</title>"
    "<style>body{font-family:monospace;background:#05050f;color:#a855f7;"
    "display:flex;flex-direction:column;align-items:center;justify-content:center;"
    "height:100vh;gap:12px}</style></head><body>"
    "<h2>&#9889; Dobby</h2>"
    "<p>chat.html not found. Place it in the assets/ directory and restart.</p>"
    "</body></html>";

/* Gateway state */

struct gateway {
    bus_t            *bus;
    scheduler_t      *scheduler;
    subagent_pool_t  *subagents;
    email_channel_t  *email;
    allowlist_t      *allowlist;
    int               port;
    int               server_fd;
    volatile bool     running;
    pthread_t         listener;
    sem_t             thread_slots;
    char             *html_path;
    bool              debug;
};

typedef struct {
    int        client_fd;
    gateway_t *gw;
    char       client_ip[INET_ADDRSTRLEN];  /* peer IP for session routing */
} conn_ctx_t;

#define GW_DBG(gw, fmt, ...) \
    do { if ((gw)->debug) fprintf(stderr, "[gateway:debug] " fmt "\n", ##__VA_ARGS__); } while(0)

/* HTML file loading */

static char *load_html(const char *path, size_t *len)
{
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) { LOG_WARN("chat.html not found at '%s' — using fallback", path); return NULL; }
    struct stat st;
    fstat(fileno(f), &st);
    if (st.st_size <= 0 || st.st_size > 2 * 1024 * 1024) {
        fclose(f);
        LOG_WARN("chat.html size out of range (%ld bytes) — using fallback", (long)st.st_size);
        return NULL;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)st.st_size, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

/* HTTP helpers */

static void send_response(int fd, int code, const char *ctype,
                          const char *body, size_t body_len)
{
    const char *reason = (code == 200) ? "OK"
                       : (code == 400) ? "Bad Request"
                       : (code == 404) ? "Not Found"
                       : (code == 503) ? "Service Unavailable"
                       :                 "Internal Server Error";
    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, reason, ctype, body_len);
    { ssize_t _r = write(fd, hdr,  (size_t)hdr_len); (void)_r; }
    { ssize_t _r = write(fd, body, body_len);         (void)_r; }
}

static void send_json(int fd, int code, const char *json)
{
    send_response(fd, code, "application/json", json, strlen(json));
}

static void send_json_obj(int fd, int code, cJSON *obj)
{
    char *json = cJSON_PrintUnformatted(obj);
    send_json(fd, code, json);
    free(json);
    cJSON_Delete(obj);
}

/* Request parsing */

static char *read_request(int fd)
{
    char *buf = malloc(GATEWAY_RECV_BUF);
    if (!buf) return NULL;
    size_t total = 0;
    ssize_t n;
    while (total < (size_t)(GATEWAY_RECV_BUF - 1)) {
        n = read(fd, buf + total, (size_t)(GATEWAY_RECV_BUF - 1) - total);
        if (n <= 0) break;
        total += (size_t)n;
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            char *cl = strcasestr(buf, "Content-Length:");
            if (!cl) break;
            long cl_val = strtol(cl + 15, NULL, 10);
            if (cl_val <= 0 || cl_val > GATEWAY_BODY_LIMIT) break;
            size_t hdr_bytes = (size_t)(hdr_end + 4 - buf);
            if (total - hdr_bytes >= (size_t)cl_val) break;
        }
    }
    buf[total] = '\0';
    if (total == 0) { free(buf); return NULL; }
    return buf;
}

static const char *request_body(const char *buf)
{
    const char *sep = strstr(buf, "\r\n\r\n");
    return sep ? sep + 4 : NULL;
}

/* Route handlers */

static void handle_status(int fd, gateway_t *gw)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status",  "ok");
    cJSON_AddStringToObject(obj, "version", VERSION_STRING);
    cJSON_AddBoolToObject(obj, "bus_ready", gw->bus != NULL);
    cJSON_AddBoolToObject(obj, "debug",     gw->debug);
    cJSON_AddNumberToObject(obj, "inbound_queue",  (double)bus_inbound_size(gw->bus));
    cJSON_AddNumberToObject(obj, "outbound_queue", (double)bus_outbound_size(gw->bus));
    send_json_obj(fd, 200, obj);
}

static void handle_chat(int fd, gateway_t *gw, const char *raw, const char *client_ip)
{
    if (!gw->bus) {
        GW_DBG(gw, "chat rejected — bus not initialised");
        send_json(fd, 503, "{\"error\":\"Bus not initialised\"}");
        return;
    }

    const char *body = request_body(raw);
    if (!body || !*body) {
        GW_DBG(gw, "chat rejected — empty body");
        send_json(fd, 400, "{\"error\":\"Empty request body\"}");
        return;
    }

    GW_DBG(gw, "request body: %.512s", body);

    cJSON *req = cJSON_Parse(body);
    if (!req) {
        send_json(fd, 400, "{\"error\":\"Invalid JSON\"}");
        return;
    }

    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(req, "message"));
    if (!msg || !*msg) {
        cJSON_Delete(req);
        send_json(fd, 400, "{\"error\":\"'message' field required\"}");
        return;
    }

    GW_DBG(gw, "message: %.256s", msg);

    /* Build response_pair so this HTTP worker thread can block waiting for reply */
    response_pair_t rp;
    response_pair_init(&rp);

    /* Embed rp pointer as hex in metadata */
    char meta[64];
    snprintf(meta, sizeof(meta), "rp=%lx", (unsigned long)(uintptr_t)&rp);

    /* Use the client IP as chat_id so each HTTP client gets its own session */
    /* chat_id = client IP so each browser/machine keeps its own session */
    const char *chat_id = (client_ip && *client_ip) ? client_ip : "unknown";
    inbound_msg_t *inbound = inbound_msg_new("http", chat_id, chat_id, msg, meta);
    cJSON_Delete(req);

    if (!inbound) {
        response_pair_destroy(&rp);
        send_json(fd, 500, "{\"error\":\"Out of memory\"}");
        return;
    }

    GW_DBG(gw, "publishing to bus…");
    bus_publish_inbound(gw->bus, inbound);   /* bus owns inbound now */

    /* Block until agent worker delivers the response */
    char *response = response_pair_wait(&rp);
    response_pair_destroy(&rp);

    GW_DBG(gw, "response (%zu chars): %.256s",
           response ? strlen(response) : 0,
           response ? response : "(null)");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "response", response ? response : "");
    free(response);
    send_json_obj(fd, 200, resp);
}

static void handle_root(int fd, gateway_t *gw)
{
    size_t html_len = 0;
    char  *html     = load_html(gw->html_path, &html_len);
    if (html) {
        GW_DBG(gw, "serving chat.html (%zu bytes)", html_len);
        send_response(fd, 200, "text/html; charset=utf-8", html, html_len);
        free(html);
    } else {
        send_response(fd, 200, "text/html; charset=utf-8",
                      FALLBACK_HTML, sizeof(FALLBACK_HTML) - 1);
    }
}

static void handle_email(int fd, gateway_t *gw)
{
    cJSON *obj = cJSON_CreateObject();
    email_channel_t *ec = gw->email;
    if (!ec) {
        cJSON_AddBoolToObject(obj, "configured", false);
    } else {
        cJSON_AddBoolToObject(obj, "configured", true);
        /* Status snapshot */
        char snap[1024] = {0};
        email_channel_status(ec, snap, sizeof(snap));
        /* Parse individual fields back for clean JSON (re-read from channel) */
        cJSON_AddStringToObject(obj, "last_from",    "");
        cJSON_AddStringToObject(obj, "last_subject", "");
        /* Access internal state via email_channel_status output */
        cJSON_AddStringToObject(obj, "status_text", snap);
    }
    /* Email allowlist */
    allowlist_t *al = gw->allowlist;
    cJSON *al_arr = cJSON_CreateArray();
    if (al && allowlist_is_enabled(al, ACL_EMAIL)) {
        int n = allowlist_rule_count(al, ACL_EMAIL);
        for (int i = 0; i < n; i++) {
            const char *p = allowlist_rule_pattern(al, ACL_EMAIL, i);
            if (p) cJSON_AddItemToArray(al_arr, cJSON_CreateString(p));
        }
    }
    cJSON_AddItemToObject(obj, "allowlist", al_arr);
    cJSON_AddBoolToObject(obj, "allowlist_enabled",
                          al ? allowlist_is_enabled(al, ACL_EMAIL) : false);
    send_json_obj(fd, 200, obj);
}

static void handle_subagents(int fd, gateway_t *gw)
{
    char *json = subagent_pool_json(gw->subagents);
    send_json(fd, 200, json ? json : "[]");
    free(json);
}

static void handle_tasks(int fd, gateway_t *gw)
{
    char *json = scheduler_task_json(gw->scheduler);
    send_json(fd, 200, json ? json : "[]");
    free(json);
}

static void handle_tmux(int fd, gateway_t *gw)
{
    (void)gw;
    const char *sock = getenv("TMUX_SOCKET");
    char cmd[256];
    if (sock && *sock)
        snprintf(cmd, sizeof(cmd), "tmux -S '%s' ls -F "
                 "'#{session_name}\t#{session_windows}\t#{session_created}' 2>&1", sock);
    else
        snprintf(cmd, sizeof(cmd),
                 "tmux ls -F '#{session_name}\t#{session_windows}\t#{session_created}' 2>&1");

    cJSON *arr = cJSON_CreateArray();
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "no server running") ||
                strstr(line, "no sessions")       ||
                strstr(line, "error connecting"))
                break;
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            char *name    = line;
            char *windows = strchr(line, '\t');
            char *created = NULL;
            if (windows) { *windows++ = '\0'; created = strchr(windows, '\t'); }
            if (created)   *created++ = '\0';
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name",    name);
            cJSON_AddStringToObject(item, "windows", windows ? windows : "");
            cJSON_AddStringToObject(item, "created", created ? created : "");
            cJSON_AddItemToArray(arr, item);
        }
        pclose(fp);
    }
    char *json = cJSON_PrintUnformatted(arr);
    send_json(fd, 200, json ? json : "[]");
    free(json);
    cJSON_Delete(arr);
}

/* Connection worker */

static void *gateway_conn_thread(void *arg)
{
    conn_ctx_t *ctx = (conn_ctx_t *)arg;
    gateway_t  *gw  = ctx->gw;
    int         fd   = ctx->client_fd;
    char        client_ip[INET_ADDRSTRLEN];
    strncpy(client_ip, ctx->client_ip, sizeof(client_ip));
    free(ctx);

    char *raw = read_request(fd);
    if (!raw) { close(fd); sem_post(&gw->thread_slots); return NULL; }

    char method[16] = {0}, path[256] = {0};
    sscanf(raw, "%15s %255s", method, path);
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    GW_DBG(gw, "→ %s %s", method, path);
    LOG_DEBUG("HTTP %s %s", method, path);

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        handle_root(fd, gw);
    } else if (strcmp(path, "/api/status") == 0) {
        handle_status(fd, gw);
    } else if (strcmp(path, "/api/chat") == 0) {
        if (strcmp(method, "POST") == 0)
            handle_chat(fd, gw, raw, client_ip);
        else if (strcmp(method, "OPTIONS") == 0)
            send_json(fd, 200, "{}");
        else
            send_json(fd, 400, "{\"error\":\"POST required\"}");
    } else if (strcmp(path, "/api/tasks") == 0) {
        handle_tasks(fd, gw);
    } else if (strcmp(path, "/api/subagents") == 0) {
        handle_subagents(fd, gw);
    } else if (strcmp(path, "/api/tmux") == 0) {
        handle_tmux(fd, gw);
    } else if (strcmp(path, "/api/email") == 0) {
        handle_email(fd, gw);
    } else if (strcmp(path, "/favicon.ico") == 0) {
        send_response(fd, 204, "image/x-icon", "", 0);
    } else {
        GW_DBG(gw, "404: %s", path);
        send_json(fd, 404, "{\"error\":\"Not found\"}");
    }

    free(raw);
    close(fd);
    sem_post(&gw->thread_slots);
    return NULL;
}

/* Listener thread */

static void *gateway_listen_thread(void *arg)
{
    gateway_t *gw = (gateway_t *)arg;
    signal(SIGPIPE, SIG_IGN);

    while (gw->running) {
        sem_wait(&gw->thread_slots);
        if (!gw->running) { sem_post(&gw->thread_slots); break; }

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(gw->server_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            sem_post(&gw->thread_slots);
            if (!gw->running) break;
            continue;
        }

        GW_DBG(gw, "accepted connection from %s:%d",
               inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

        conn_ctx_t *ctx = malloc(sizeof(conn_ctx_t));
        if (!ctx) { close(client_fd); sem_post(&gw->thread_slots); continue; }
        ctx->client_fd = client_fd;
        ctx->gw        = gw;
        strncpy(ctx->client_ip, inet_ntoa(peer.sin_addr), INET_ADDRSTRLEN - 1);
        ctx->client_ip[INET_ADDRSTRLEN - 1] = '\0';

        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, gateway_conn_thread, ctx) != 0) {
            free(ctx); close(client_fd); sem_post(&gw->thread_slots);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* Public API */

gateway_t *gateway_create(bus_t *bus, scheduler_t *scheduler,
                           subagent_pool_t *subagents,
                           int port, const char *html_path, bool debug,
                           email_channel_t *email, allowlist_t *allowlist)
{
    if (!debug) {
        const char *env = getenv("DEBUG");
        if (env && (!strcmp(env, "1") || !strcasecmp(env, "true")))
            debug = true;
    }
    gateway_t *gw  = calloc(1, sizeof(gateway_t));
    gw->bus        = bus;
    gw->scheduler  = scheduler;
    gw->subagents  = subagents;
    gw->email      = email;
    gw->allowlist  = allowlist;
    gw->port       = port > 0 ? port : 8080;
    gw->debug      = debug;
    gw->html_path  = html_path ? strdup(html_path) : NULL;
    sem_init(&gw->thread_slots, 0, GATEWAY_MAX_THREADS);

    if (debug)
        fprintf(stderr, "[gateway:debug] debug mode ON\n");
    return gw;
}

result_t gateway_start(gateway_t *gw)
{
    gw->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gw->server_fd < 0)
        return err(ERR_IO, "socket(): %s", strerror(errno));

    int opt = 1;
    setsockopt(gw->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons((uint16_t)gw->port),
    };
    if (bind(gw->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(gw->server_fd);
        return err(ERR_IO, "bind(:%d): %s", gw->port, strerror(errno));
    }
    if (listen(gw->server_fd, GATEWAY_BACKLOG) < 0) {
        close(gw->server_fd);
        return err(ERR_IO, "listen(): %s", strerror(errno));
    }

    gw->running = true;
    pthread_create(&gw->listener, NULL, gateway_listen_thread, gw);

    fprintf(stderr,
        "\n  ⚡ Gateway  : http://0.0.0.0:%d\n"
        "     Web UI  : http://<your-ip>:%d/\n"
        "     API     : POST http://<your-ip>:%d/api/chat\n"
        "     Debug   : %s\n\n",
        gw->port, gw->port, gw->port,
        gw->debug ? "ON (--debug)" : "off  (use --debug or DEBUG=1 to enable)");

    return ok();
}

void gateway_destroy(gateway_t *gw)
{
    if (!gw) return;
    gw->running = false;
    shutdown(gw->server_fd, SHUT_RDWR);
    close(gw->server_fd);
    pthread_join(gw->listener, NULL);
    sem_destroy(&gw->thread_slots);
    free(gw->html_path);
    free(gw);
}
