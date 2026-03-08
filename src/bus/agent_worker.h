/*
 * agent_worker.h — Single-threaded agent worker over the bus
 *
 * Drains the bus inbound queue, routes each message to the correct
 * session (via session_manager), calls agent_chat(), then either:
 *   - delivers the response directly via response_pair if "rp=<hex>"
 *     is present in the message metadata (synchronous CLI/HTTP callers), or
 *   - posts an outbound_msg_t to the bus for async channels.
 *
 * Only one worker thread is needed — agent_chat() is serialised per session
 * and sessions are independent, so this is effectively single-threaded per
 * conversation but concurrent across conversations.
 *
 * For now a single worker serialises ALL sessions (simplest, avoids per-session
 * locking).  If throughput matters later, a pool can be added here.
 */
#ifndef AGENT_WORKER_H
#define AGENT_WORKER_H

#include "bus.h"
#include "../session/session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_worker agent_worker_t;

/*
 * Create the worker.
 * bus and sm must outlive the worker.
 */
agent_worker_t *agent_worker_create(bus_t             *bus,
                                     session_manager_t *sm);
void            agent_worker_destroy(agent_worker_t *w);

/* Start the background worker thread. */
void agent_worker_start(agent_worker_t *w);

/* Signal stop and join the thread. */
void agent_worker_stop(agent_worker_t *w);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_WORKER_H */
