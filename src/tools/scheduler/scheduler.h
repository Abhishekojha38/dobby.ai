/*
 * scheduler.h — Cron-style task scheduler
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "../tool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scheduler scheduler_t;

/* Create the scheduler. Starts a background thread. */
scheduler_t *scheduler_create(void);

/* Register scheduler tools with the tool registry. */
void scheduler_register_tools(scheduler_t *sched);

/*
 * Fill buf with a human-readable table of active tasks (thread-safe).
 * Used by the /tasks slash command.
 */
void scheduler_task_snapshot(scheduler_t *sched,
                                   char *buf, size_t buf_size);

/*
 * Return a heap-allocated JSON string (cJSON array) of active tasks.
 * Caller must free(). Used by GET /api/tasks in the gateway.
 * Returns an empty JSON array "[]" when sched is NULL or no tasks exist.
 */
char *scheduler_task_json(scheduler_t *sched);

/* Destroy the scheduler, stopping all tasks. */
void scheduler_destroy(scheduler_t *sched);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_H */
