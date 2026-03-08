/*
 * gateway.h — HTTP server: REST API + web chat UI
 *
 * Endpoints:
 *   GET  /            — Web chat UI (from chat.html or built-in fallback)
 *   GET  /api/status  — JSON health + version
 *   POST /api/chat    — JSON chat  { "message": "..." } → { "response": "..." }
 *   GET  /api/tasks   — JSON array of active background scheduler tasks
 *   GET  /api/tmux    — JSON array of active tmux sessions
 *   GET  /api/email   — JSON email channel status + last sender info
 */
#ifndef GATEWAY_H
#define GATEWAY_H

#include "../core/dobby.h"
#include "../bus/bus.h"
#include "../tools/scheduler/scheduler.h"
#include "../channels/email/email_channel.h"
#include "../security/allowlist.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gateway gateway_t;

/*
 * Create a gateway.
 *
 * @bus:       Bus instance. HTTP /api/chat posts inbound messages here.
 * @scheduler: Scheduler for /api/tasks. NULL → returns empty array.
 * @port:      TCP port. 0 → 8080.
 * @html_path: Path to chat.html. NULL or missing → built-in minimal fallback.
 * @debug:     true → verbose per-request logging. Also enabled by DEBUG=1 env.
 * @email:     Email channel for /api/email. NULL → returns disabled status.
 * @allowlist: Allowlist for /api/email allowlist data. NULL → omitted.
 */
gateway_t *gateway_create(bus_t           *bus,
                           scheduler_t     *scheduler,
                           int              port,
                           const char      *html_path,
                           bool             debug,
                           email_channel_t *email,
                           allowlist_t     *allowlist);

/* Start listening (non-blocking, spawns listener thread). */
result_t gateway_start(gateway_t *gw);

/* Graceful shutdown. */
void gateway_destroy(gateway_t *gw);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_H */
