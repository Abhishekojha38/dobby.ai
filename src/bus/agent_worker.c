/*
 * agent_worker.c — Agent worker thread
 */
#include "agent_worker.h"
#include "../core/log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct agent_worker {
    bus_t             *bus;
    session_manager_t *sm;
    pthread_t          thread;
    volatile bool      running;
};

static void *worker_loop(void *arg)
{
    agent_worker_t *w = (agent_worker_t *)arg;

    LOG_DEBUG("Agent worker started");

    while (w->running) {
        inbound_msg_t *msg = bus_consume_inbound(w->bus);
        if (!msg) break;   /* bus shutdown */

        LOG_DEBUG("Worker: channel='%s' chat='%s' content_len=%zu",
                  msg->channel, msg->chat_id, strlen(msg->content));

        /* Look up or create the session for this (channel, chat_id) */
        session_t *sess = session_get_or_create(w->sm, msg->channel, msg->chat_id);
        char *response = NULL;

        if (sess) {
            response = agent_chat(sess->agent, msg->content);
            session_release(w->sm, sess);
        } else {
            LOG_WARN("Worker: no session available for '%s:%s'",
                     msg->channel, msg->chat_id);
            response = strdup("Error: agent unavailable (session table full).");
        }

        /* Check for synchronous response_pair in metadata */
        response_pair_t *rp = response_pair_from_meta(msg->metadata);
        if (rp) {
            /* Synchronous caller is blocked in response_pair_wait() — wake it */
            response_pair_deliver(rp, response);   /* transfers ownership */
        } else {
            /* Async channel: post response to outbound queue */
            outbound_msg_t *out = outbound_msg_new(msg->channel,
                                                    msg->chat_id,
                                                    response ? response : "",
                                                    NULL);
            free(response);
            if (out) bus_publish_outbound(w->bus, out);
        }

        inbound_msg_free(msg);
    }

    LOG_DEBUG("Agent worker stopped");
    return NULL;
}

agent_worker_t *agent_worker_create(bus_t *bus, session_manager_t *sm)
{
    if (!bus || !sm) return NULL;
    agent_worker_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->bus = bus;
    w->sm  = sm;
    return w;
}

void agent_worker_start(agent_worker_t *w)
{
    if (!w) return;
    w->running = true;
    pthread_create(&w->thread, NULL, worker_loop, w);
}

void agent_worker_stop(agent_worker_t *w)
{
    if (!w) return;
    w->running = false;
    bus_shutdown(w->bus);   /* unblocks bus_consume_inbound */
    pthread_join(w->thread, NULL);
}

void agent_worker_destroy(agent_worker_t *w)
{
    if (!w) return;
    agent_worker_stop(w);
    free(w);
}
