/*
 * scheduler.h — Cron-style task scheduler
 *
 * Supports two task types:
 *
 *   shell  — runs a shell command via fork/exec (original behaviour)
 *   agent  — posts a natural-language message to the agent via the bus;
 *            the agent processes it as a normal conversation turn and can
 *            call any tool (send_email, memory_store, file_write, etc.)
 *
 * The agent task type solves the fundamental problem that shell commands
 * cannot invoke Dobby tools — tools are in-process functions, not binaries.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "../tool.h"
#include "../../bus/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scheduler scheduler_t;

/*
 * Create the scheduler.
 * bus: required for agent-type tasks; pass NULL to disable agent tasks.
 * Starts a background thread.
 */
scheduler_t *scheduler_create(bus_t *bus, const char *workspace);

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
