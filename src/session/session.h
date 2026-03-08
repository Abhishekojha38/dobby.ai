/*
 * session.h — Per-channel session manager
 *
 * Each unique (channel, chat_id) pair gets its own agent_t so conversations
 * are fully isolated.  Sessions are created on first message and evicted
 * after idle TTL seconds.
 *
 * Key format: "<channel>:<chat_id>"  e.g. "cli:local", "http:192.168.1.5"
 *
 * Thread-safe: all public functions hold an internal mutex.
 */
#ifndef SESSION_H
#define SESSION_H

#include "../agent/agent.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SESSION_KEY_MAX     320   /* len("channel") + 1 + len("chat_id") */
#define SESSION_MAX         64    /* max concurrent sessions               */
#define SESSION_TTL_DEFAULT 3600  /* evict after 1 h idle                  */

typedef struct {
    char     key[SESSION_KEY_MAX];
    agent_t *agent;
    time_t   last_active;
    bool     in_use;
} session_t;

typedef struct session_manager session_manager_t;

/* Factory: called to create a fresh agent_t for each new session.
 * data is the pointer passed to session_manager_create(). */
typedef agent_t *(*agent_factory_fn)(void *data);

/*
 * Create a session manager.
 * factory        — called once per new session to allocate its agent
 * factory_data   — opaque pointer forwarded to factory
 * ttl_seconds    — idle eviction timeout; <= 0 uses SESSION_TTL_DEFAULT
 */
session_manager_t *session_manager_create(agent_factory_fn factory,
                                           void            *factory_data,
                                           int              ttl_seconds);
void               session_manager_destroy(session_manager_t *sm);

/*
 * Get or create a session for (channel, chat_id).
 * Returns NULL only on allocation failure or full table (after eviction retry).
 * Caller must call session_release() after using the session.
 */
session_t *session_get_or_create(session_manager_t *sm,
                                  const char        *channel,
                                  const char        *chat_id);

/* Touch last_active after a turn completes. */
void session_release(session_manager_t *sm, session_t *session);

/* Evict sessions idle >= ttl.  Returns count evicted. */
int session_evict_stale(session_manager_t *sm);

/* Current active session count. */
int session_count(session_manager_t *sm);

/* Write a human-readable snapshot of all sessions into buf.
 * Returns number of active sessions. Thread-safe. */
int session_snapshot(session_manager_t *sm, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_H */
