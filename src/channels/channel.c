/*
 * channel.c — Channel registry and outbound dispatcher
 *
 * The agent worker lives in bus/agent_worker.c.
 * This file owns:
 *   - channel registry  (register / find / count)
 *   - outbound dispatcher thread  (routes bus outbound → channel->send())
 *   - worker_start / worker_stop  (convenience wrappers used by main.c)
 */
#include "channel.h"
#include "../bus/agent_worker.h"
#include "../core/log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ── Registry ───────────────────────────────────────────────────── */

static channel_t       *g_channels[CHANNEL_REG_MAX];
static int              g_nchannels = 0;
static pthread_mutex_t  g_reg_lock  = PTHREAD_MUTEX_INITIALIZER;

void channel_register(channel_t *ch) {
    if (!ch || !ch->name[0]) return;
    pthread_mutex_lock(&g_reg_lock);
    if (g_nchannels >= CHANNEL_REG_MAX) {
        pthread_mutex_unlock(&g_reg_lock);
        LOG_WARN("Channel registry full, cannot register '%s'", ch->name);
        return;
    }
    for (int i = 0; i < g_nchannels; i++)
        if (strcmp(g_channels[i]->name, ch->name) == 0)
            { pthread_mutex_unlock(&g_reg_lock); return; }
    g_channels[g_nchannels++] = ch;
    pthread_mutex_unlock(&g_reg_lock);
    LOG_DEBUG("Channel registered: '%s'", ch->name);
}

channel_t *channel_find(const char *name) {
    if (!name) return NULL;
    pthread_mutex_lock(&g_reg_lock);
    channel_t *found = NULL;
    for (int i = 0; i < g_nchannels; i++)
        if (strcmp(g_channels[i]->name, name) == 0) { found = g_channels[i]; break; }
    pthread_mutex_unlock(&g_reg_lock);
    return found;
}

int channel_count(void) { return g_nchannels; }

/* ── Outbound dispatcher ────────────────────────────────────────── */

static pthread_t      g_disp_tid;
static volatile bool  g_disp_running = false;
static bus_t         *g_disp_bus     = NULL;

static void *dispatcher_fn(void *arg) {
    bus_t *bus = (bus_t *)arg;
    LOG_DEBUG("Dispatcher thread started");
    while (true) {
        outbound_msg_t *msg = bus_consume_outbound(bus);
        if (!msg) break;
        channel_t *ch = channel_find(msg->channel);
        if (ch && ch->send)
            ch->send(ch, msg);
        else
            LOG_WARN("Dispatcher: unknown channel '%s'", msg->channel);
        outbound_msg_free(msg);
    }
    LOG_DEBUG("Dispatcher thread exiting");
    return NULL;
}

/* ── Worker lifecycle (wraps agent_worker + dispatcher) ─────────── */

static agent_worker_t *g_worker = NULL;

bool worker_start(bus_t *bus, session_manager_t *sm) {
    if (g_worker) return true;

    g_worker = agent_worker_create(bus, sm);
    if (!g_worker) { LOG_WARN("Failed to create agent worker"); return false; }
    agent_worker_start(g_worker);

    g_disp_bus = bus;
    if (pthread_create(&g_disp_tid, NULL, dispatcher_fn, bus) != 0) {
        LOG_WARN("Failed to create dispatcher thread");
        agent_worker_destroy(g_worker); g_worker = NULL;
        return false;
    }
    g_disp_running = true;
    LOG_DEBUG("Worker + dispatcher started");
    return true;
}

void worker_stop(void) {
    if (!g_worker) return;
    agent_worker_destroy(g_worker);   /* shuts down bus, joins worker thread */
    g_worker = NULL;
    if (g_disp_running) {
        pthread_join(g_disp_tid, NULL);
        g_disp_running = false;
    }
    LOG_DEBUG("Worker + dispatcher stopped");
}

bool channels_start_threads(bus_t *bus, session_manager_t *sm) { return worker_start(bus, sm); }
void channels_stop_threads(void) { worker_stop(); }
