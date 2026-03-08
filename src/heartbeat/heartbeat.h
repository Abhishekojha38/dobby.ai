/*
 * heartbeat.h — Proactive heartbeat system
 */
#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "../core/dobby.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct heartbeat heartbeat_t;

/* Callback for heartbeat events. Return non-NULL message to notify user (caller frees). */
typedef char *(*heartbeat_check_fn)(void *user_data);

/* Create heartbeat system with interval in seconds. */
heartbeat_t *heartbeat_create(int interval_sec);

/* Register a check function to be called on each heartbeat. */
void heartbeat_add_check(heartbeat_t *hb,
                                const char *name,
                                heartbeat_check_fn fn,
                                void *user_data);

/* Set callback for when a notification should be shown to the user. */
void heartbeat_set_notify(heartbeat_t *hb,
                                 void (*notify)(const char *msg, void *data),
                                 void *user_data);

/* Start the heartbeat thread. */
void heartbeat_start(heartbeat_t *hb);

/* Stop and destroy. */
void heartbeat_destroy(heartbeat_t *hb);

#ifdef __cplusplus
}
#endif

#endif /* HEARTBEAT_H */
