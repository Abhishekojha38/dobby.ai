/*
 * session.c — Per-channel session manager
 *
 * Fixed-size open-addressing hash table (linear probing, djb2).
 * Low load factor by design — SESSION_MAX slots, only 64 by default.
 */
#include "session.h"
#include "../agent/agent.h"
#include "../core/log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned int djb2(const char *s)
{
    unsigned int h = 5381;
    while (*s) h = h * 33 ^ (unsigned char)*s++;
    return h;
}

struct session_manager {
    session_t        slots[SESSION_MAX];
    pthread_mutex_t  lock;
    agent_factory_fn factory;
    void            *factory_data;
    int              ttl_seconds;
    int              count;
};

session_manager_t *session_manager_create(agent_factory_fn factory,
                                           void *factory_data,
                                           int   ttl_seconds)
{
    session_manager_t *sm = calloc(1, sizeof(*sm));
    if (!sm) return NULL;
    pthread_mutex_init(&sm->lock, NULL);
    sm->factory      = factory;
    sm->factory_data = factory_data;
    sm->ttl_seconds  = ttl_seconds > 0 ? ttl_seconds : SESSION_TTL_DEFAULT;
    LOG_DEBUG("Session manager created (max=%d ttl=%ds)", SESSION_MAX, sm->ttl_seconds);
    return sm;
}

void session_manager_destroy(session_manager_t *sm)
{
    if (!sm) return;
    pthread_mutex_lock(&sm->lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (sm->slots[i].in_use) {
            agent_destroy(sm->slots[i].agent);
            sm->slots[i].in_use = false;
        }
    }
    pthread_mutex_unlock(&sm->lock);
    pthread_mutex_destroy(&sm->lock);
    free(sm);
}

/* Caller must hold lock.  Returns slot index (existing or empty) or -1. */
static int find_slot(session_manager_t *sm, const char *key)
{
    unsigned int start = djb2(key) % SESSION_MAX;
    int first_empty = -1;
    for (int i = 0; i < SESSION_MAX; i++) {
        int idx = (int)((start + (unsigned)i) % SESSION_MAX);
        session_t *s = &sm->slots[idx];
        if (s->in_use && strcmp(s->key, key) == 0) return idx;
        if (!s->in_use && first_empty < 0) first_empty = idx;
    }
    return first_empty;   /* -1 only if every slot is in_use */
}

/* Evict stale sessions without acquiring the lock (caller holds it). */
static int evict_locked(session_manager_t *sm)
{
    time_t now = time(NULL);
    int evicted = 0;
    for (int i = 0; i < SESSION_MAX; i++) {
        session_t *s = &sm->slots[i];
        if (!s->in_use) continue;
        if (difftime(now, s->last_active) >= (double)sm->ttl_seconds) {
            LOG_DEBUG("Evicting session '%s'", s->key);
            agent_destroy(s->agent);
            memset(s, 0, sizeof(*s));
            sm->count--;
            evicted++;
        }
    }
    return evicted;
}

session_t *session_get_or_create(session_manager_t *sm,
                                  const char        *channel,
                                  const char        *chat_id)
{
    if (!sm || !channel || !chat_id) return NULL;

    char key[SESSION_KEY_MAX];
    snprintf(key, sizeof(key), "%s:%s", channel, chat_id);

    pthread_mutex_lock(&sm->lock);

    int idx = find_slot(sm, key);
    if (idx < 0) {
        /* Table full — evict stale sessions and retry once */
        evict_locked(sm);
        idx = find_slot(sm, key);
        if (idx < 0) {
            pthread_mutex_unlock(&sm->lock);
            LOG_WARN("Session table full for '%s' — cannot create session", key);
            return NULL;
        }
    }

    session_t *s = &sm->slots[idx];

    if (!s->in_use) {
        /* New session — call factory outside the hot path if needed,
         * but factory must be fast (it just creates an agent_t struct). */
        agent_t *agent = sm->factory(sm->factory_data);
        if (!agent) {
            pthread_mutex_unlock(&sm->lock);
            LOG_WARN("Agent factory failed for session '%s'", key);
            return NULL;
        }
        strncpy(s->key, key, SESSION_KEY_MAX - 1);
        s->key[SESSION_KEY_MAX - 1] = '\0';
        s->agent       = agent;
        s->in_use      = true;
        s->last_active = time(NULL);
        sm->count++;
        LOG_DEBUG("Session created: '%s' (total=%d)", key, sm->count);
    }

    pthread_mutex_unlock(&sm->lock);
    return s;
}

void session_release(session_manager_t *sm, session_t *session)
{
    if (!sm || !session) return;
    pthread_mutex_lock(&sm->lock);
    session->last_active = time(NULL);
    pthread_mutex_unlock(&sm->lock);
}

int session_evict_stale(session_manager_t *sm)
{
    if (!sm) return 0;
    pthread_mutex_lock(&sm->lock);
    int n = evict_locked(sm);
    pthread_mutex_unlock(&sm->lock);
    if (n) LOG_DEBUG("Evicted %d stale session(s)", n);
    return n;
}

int session_count(session_manager_t *sm)
{
    if (!sm) return 0;
    pthread_mutex_lock(&sm->lock);
    int n = sm->count;
    pthread_mutex_unlock(&sm->lock);
    return n;
}

int session_snapshot(session_manager_t *sm, char *buf, size_t buf_size)
{
    if (!sm || !buf || !buf_size) return 0;
    buf[0] = '\0';
    int found = 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&sm->lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        session_t *s = &sm->slots[i];
        if (!s->in_use) continue;
        long idle = (long)difftime(now, s->last_active);
        int msgs   = s->agent ? agent_message_count(s->agent) : 0;
        size_t used = strlen(buf);
        snprintf(buf + used, buf_size - used,
                 "  %-36s  msgs:%-4d  idle:%lds\n",
                 s->key, msgs, idle);
        found++;
    }
    pthread_mutex_unlock(&sm->lock);
    return found;
}
