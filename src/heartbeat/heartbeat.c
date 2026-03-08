/*
 * heartbeat.c — Proactive heartbeat loop
 *
 * Runs a background thread that calls registered check functions
 * at regular intervals. If a check returns a non-NULL message,
 * the user is notified.
 */
#include "heartbeat.h"
#include "../core/log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CHECKS 16

typedef struct {
    char                   *name;
    heartbeat_check_fn fn;
    void                   *user_data;
} heartbeat_check_t;

struct heartbeat {
    int               interval_sec;
    heartbeat_check_t checks[MAX_CHECKS];
    int               check_count;
    pthread_t         thread;
    bool              running;

    void (*notify)(const char *msg, void *data);
    void  *notify_data;
};

static void *heartbeat_thread(void *arg) {
    heartbeat_t *hb = (heartbeat_t *)arg;

    while (hb->running) {
        sleep((unsigned int)hb->interval_sec);
        if (!hb->running) break;

        for (int i = 0; i < hb->check_count; i++) {
            char *msg = hb->checks[i].fn(hb->checks[i].user_data);
            if (msg) {
                LOG_DEBUG("Heartbeat [%s]: %s", hb->checks[i].name, msg);
                if (hb->notify) {
                    hb->notify(msg, hb->notify_data);
                }
                free(msg);
            }
        }
    }

    return NULL;
}

heartbeat_t *heartbeat_create(int interval_sec) {
    heartbeat_t *hb = calloc(1, sizeof(heartbeat_t));
    hb->interval_sec = interval_sec > 0 ? interval_sec : 60;
    hb->running = false;
    LOG_DEBUG("Heartbeat created (interval: %ds)", hb->interval_sec);
    return hb;
}

void heartbeat_add_check(heartbeat_t *hb, const char *name,
                                heartbeat_check_fn fn, void *user_data) {
    if (hb->check_count >= MAX_CHECKS) return;
    heartbeat_check_t *c = &hb->checks[hb->check_count++];
    c->name      = strdup(name);
    c->fn        = fn;
    c->user_data = user_data;
    LOG_DEBUG("Heartbeat check added: %s", name);
}

void heartbeat_set_notify(heartbeat_t *hb,
                                 void (*notify)(const char *msg, void *data),
                                 void *user_data) {
    hb->notify      = notify;
    hb->notify_data = user_data;
}

void heartbeat_start(heartbeat_t *hb) {
    if (hb->running) return;
    hb->running = true;
    pthread_create(&hb->thread, NULL, heartbeat_thread, hb);
    LOG_DEBUG("Heartbeat started");
}

void heartbeat_destroy(heartbeat_t *hb) {
    if (!hb) return;
    if (hb->running) {
        hb->running = false;
        pthread_join(hb->thread, NULL);
    }
    for (int i = 0; i < hb->check_count; i++) {
        free(hb->checks[i].name);
    }
    free(hb);
    LOG_DEBUG("Heartbeat destroyed");
}
