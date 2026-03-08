/*
 * bus.h — Dobby message bus
 *
 * Decouples channels (CLI, HTTP, …) from the agent worker thread.
 *
 * ── Flow ────────────────────────────────────────────────────────
 *
 *   Async channels (fire-and-forget style):
 *     channel  → bus_publish_inbound()  → [inbound queue]
 *                                           ↓  agent worker drains
 *                bus_publish_outbound() ← (worker posts reply)
 *     channel  ← dispatcher drains outbound, routes back to channel
 *
 *   Sync callers (CLI thread, HTTP worker thread):
 *     Embed a response_pair_t* in inbound metadata ("rp=<hex ptr>").
 *     Agent worker sees the tag, calls response_pair_deliver() instead of
 *     posting to outbound queue.  Caller unblocks from response_pair_wait().
 *     This avoids needing a per-channel dispatcher for synchronous use.
 *
 * ── Ownership ───────────────────────────────────────────────────
 *   bus_publish_*   takes ownership of the msg pointer (frees on error/shutdown).
 *   bus_consume_*   transfers ownership to the caller.
 *   Always free with the matching *_msg_free().
 */
#ifndef BUS_H
#define BUS_H

#include <pthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Messages ───────────────────────────────────────────────────── */

/*
 * inbound_msg_t — channel → agent
 *
 *   channel   : "cli", "http", "email", …
 *   sender_id : channel-specific sender id  ("local", "192.168.1.5:4321")
 *   chat_id   : conversation identity; used with channel as session key
 *   content   : UTF-8 message text
 *   metadata  : optional JSON string, or special tags like "rp=0xADDR";
 *               NULL when unused
 */
typedef struct {
    char *channel;
    char *sender_id;
    char *chat_id;
    char *content;
    char *metadata;
} inbound_msg_t;

/*
 * outbound_msg_t — agent → channel
 *
 *   channel  : destination channel
 *   chat_id  : destination conversation
 *   content  : response text
 *   metadata : optional JSON for channel-specific routing; NULL when unused
 */
typedef struct {
    char *channel;
    char *chat_id;
    char *content;
    char *metadata;
} outbound_msg_t;

/* ── Message lifecycle ──────────────────────────────────────────── */

inbound_msg_t  *inbound_msg_new(const char *channel,
                                 const char *sender_id,
                                 const char *chat_id,
                                 const char *content,
                                 const char *metadata);
void            inbound_msg_free(inbound_msg_t *msg);

outbound_msg_t *outbound_msg_new(const char *channel,
                                  const char *chat_id,
                                  const char *content,
                                  const char *metadata);
void            outbound_msg_free(outbound_msg_t *msg);

/* ── Bus ────────────────────────────────────────────────────────── */

typedef struct bus bus_t;

bus_t *bus_create(void);
void   bus_destroy(bus_t *bus);

/* Unblock all waiters and refuse further publishes.
 * Call before bus_destroy() during shutdown. */
void bus_shutdown(bus_t *bus);

/* Publish: takes ownership of msg.
 * Blocks when queue is full.  Returns false (and frees msg) on shutdown. */
bool bus_publish_inbound (bus_t *bus, inbound_msg_t  *msg);
bool bus_publish_outbound(bus_t *bus, outbound_msg_t *msg);

/* Consume: blocks until available or shutdown.
 * Returns NULL on shutdown.  Caller owns the returned pointer. */
inbound_msg_t  *bus_consume_inbound (bus_t *bus);
outbound_msg_t *bus_consume_outbound(bus_t *bus);

/* Snapshot queue depths (non-atomic, diagnostic only). */
int bus_inbound_size (bus_t *bus);
int bus_outbound_size(bus_t *bus);

/* Diagnostic queue depths. */
int bus_inbound_size (bus_t *bus);
int bus_outbound_size(bus_t *bus);

/* ── Response pair (synchronous callers) ────────────────────────── */

/*
 * Callers that need a synchronous reply (CLI REPL, HTTP worker) allocate a
 * response_pair_t on the stack, embed its address as "rp=<hex>" in the
 * inbound message metadata, then call response_pair_wait().
 *
 * The agent worker calls response_pair_from_meta() to extract the pointer,
 * then response_pair_deliver() with the heap-allocated reply string.
 * The caller's thread is unblocked and owns the response string.
 *
 * Stack allocation is safe: the caller blocks until delivery, so the
 * response_pair_t lifetime always exceeds the agent worker's access window.
 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  ready;
    char           *response;   /* heap-allocated; caller frees */
    bool            done;
} response_pair_t;

void  response_pair_init   (response_pair_t *rp);
void  response_pair_destroy(response_pair_t *rp);

/* Worker calls this to unblock the caller.  Takes ownership of resp. */
void  response_pair_deliver(response_pair_t *rp, char *resp);

/* Caller blocks here until response_pair_deliver() is called.
 * Returns the heap-allocated response string; caller frees. */
char *response_pair_wait(response_pair_t *rp);

/* Parse "rp=<hex>" out of a metadata string.
 * Returns the response_pair_t pointer, or NULL if tag absent/invalid. */
response_pair_t *response_pair_from_meta(const char *metadata);

#ifdef __cplusplus
}
#endif

#endif /* BUS_H */
