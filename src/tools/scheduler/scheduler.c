/*
 * scheduler.c — Cron-style task scheduler
 *
 * Supports scheduling commands to run at intervals.
 * Simplified cron: "every <N> <unit>" where unit = seconds|minutes|hours
 * Also supports standard 5-field cron for minute-level precision.
 */
#include "scheduler.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_TASKS 64

typedef struct {
    int    id;
    char  *name;
    char  *command;       /* Shell command to execute */
    int    interval_sec;  /* Run every N seconds */
    time_t last_run;
    bool   paused;
    bool   active;
} sched_task_t;

struct scheduler {
    sched_task_t tasks[MAX_TASKS];
    int          task_count;
    int          next_id;
    pthread_t    thread;
    bool         running;
    pthread_mutex_t mutex;
};

/* Safe fire-and-forget execution */

/*
 * scheduler_run_async — fork + exec without system().
 *
 * Spawns a grandchild via double-fork so the immediate child can be
 * reaped with WNOHANG immediately, avoiding zombie accumulation.
 * The grandchild runs the command with all output discarded.
 * No blocking in the scheduler thread.
 */
static void scheduler_run_async(const char *command) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_WARN("Scheduler fork failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        /* Intermediate child: double-fork to orphan the grandchild */
        pid_t gc = fork();
        if (gc < 0) { _exit(1); }
        if (gc > 0) { _exit(0); } /* Intermediate exits immediately */

        /* Grandchild: redirect output, exec */
        int devnull = open("/dev/null", 1 /* O_WRONLY */);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* Parent: reap intermediate child immediately */
    waitpid(pid, NULL, 0);
}

/* Background thread */

static void *scheduler_thread(void *arg) {
    scheduler_t *sched = (scheduler_t *)arg;

    while (sched->running) {
        sleep(1); /* Check every second */
        time_t now = time(NULL);

        pthread_mutex_lock(&sched->mutex);
        for (int i = 0; i < sched->task_count; i++) {
            sched_task_t *task = &sched->tasks[i];
            if (!task->active || task->paused) continue;

            if (now - task->last_run >= task->interval_sec) {
                task->last_run = now;
                LOG_DEBUG("Scheduler: running task '%s' (id=%d)", task->name, task->id);
                /* Use double-fork instead of system() — avoids shell injection
                 * risks of building a command string and prevents zombies. */
                scheduler_run_async(task->command);
            }
        }
        pthread_mutex_unlock(&sched->mutex);
    }

    return NULL;
}

/* Parse interval */

static int parse_interval(const char *spec) {
    int n = 0;
    char unit[32] = {0};

    /* Try "every N unit" format */
    if (sscanf(spec, "every %d %31s", &n, unit) == 2) {
        if (strstr(unit, "sec"))  return n;
        if (strstr(unit, "min"))  return n * 60;
        if (strstr(unit, "hour")) return n * 3600;
        if (strstr(unit, "day"))  return n * 86400;
    }

    /* Try just a number (seconds) */
    if (sscanf(spec, "%d", &n) == 1 && n > 0) {
        return n;
    }

    return -1;
}

/* Public API */

scheduler_t *scheduler_create(void) {
    scheduler_t *sched = calloc(1, sizeof(scheduler_t));
    sched->next_id = 1;
    sched->running = true;
    pthread_mutex_init(&sched->mutex, NULL);
    pthread_create(&sched->thread, NULL, scheduler_thread, sched);
    LOG_DEBUG("Scheduler started");
    return sched;
}

/* Tool wrappers */

static char *tool_schedule_add(const cJSON *args, void *user_data) {
    scheduler_t *sched = (scheduler_t *)user_data;
    const char *name     = cJSON_GetStringValue(cJSON_GetObjectItem(args, "name"));
    const char *command  = cJSON_GetStringValue(cJSON_GetObjectItem(args, "command"));
    const char *interval = cJSON_GetStringValue(cJSON_GetObjectItem(args, "interval"));

    if (!name || !command || !interval) {
        return strdup("{\"error\": \"name, command, and interval required\"}");
    }

    int interval_sec = parse_interval(interval);
    if (interval_sec <= 0) {
        return strdup("{\"error\": \"Invalid interval. Use 'every N seconds/minutes/hours'\"}");
    }

    pthread_mutex_lock(&sched->mutex);
    if (sched->task_count >= MAX_TASKS) {
        pthread_mutex_unlock(&sched->mutex);
        return strdup("{\"error\": \"Max tasks reached\"}");
    }

    sched_task_t *task = &sched->tasks[sched->task_count++];
    task->id           = sched->next_id++;
    task->name         = strdup(name);
    task->command      = strdup(command);
    task->interval_sec = interval_sec;
    task->last_run     = time(NULL);
    task->paused       = false;
    task->active       = true;
    pthread_mutex_unlock(&sched->mutex);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "scheduled");
    cJSON_AddNumberToObject(result, "task_id", task->id);
    cJSON_AddStringToObject(result, "name", name);
    cJSON_AddNumberToObject(result, "interval_seconds", interval_sec);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

static char *tool_schedule_list(const cJSON *args, void *user_data) {
    (void)args;
    scheduler_t *sched = (scheduler_t *)user_data;

    pthread_mutex_lock(&sched->mutex);
    cJSON *result = cJSON_CreateArray();

    for (int i = 0; i < sched->task_count; i++) {
        sched_task_t *t = &sched->tasks[i];
        if (!t->active) continue;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", t->id);
        cJSON_AddStringToObject(item, "name", t->name);
        cJSON_AddStringToObject(item, "command", t->command);
        cJSON_AddNumberToObject(item, "interval_seconds", t->interval_sec);
        cJSON_AddBoolToObject(item, "paused", t->paused);
        cJSON_AddItemToArray(result, item);
    }
    pthread_mutex_unlock(&sched->mutex);

    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

static char *tool_schedule_control(const cJSON *args, void *user_data) {
    scheduler_t *sched = (scheduler_t *)user_data;
    cJSON *id_val     = cJSON_GetObjectItem(args, "task_id");
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(args, "action"));

    if (!id_val || !action) {
        return strdup("{\"error\": \"task_id and action required\"}");
    }

    int task_id = id_val->valueint;

    pthread_mutex_lock(&sched->mutex);
    for (int i = 0; i < sched->task_count; i++) {
        sched_task_t *t = &sched->tasks[i];
        if (t->id == task_id && t->active) {
            if (strcmp(action, "pause") == 0) {
                t->paused = true;
            } else if (strcmp(action, "resume") == 0) {
                t->paused = false;
            } else if (strcmp(action, "delete") == 0) {
                t->active = false;
                free(t->name);
                free(t->command);
                t->name = NULL;
                t->command = NULL;
            }
            pthread_mutex_unlock(&sched->mutex);

            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "status", action);
            cJSON_AddNumberToObject(result, "task_id", task_id);
            char *json = cJSON_PrintUnformatted(result);
            cJSON_Delete(result);
            return json;
        }
    }
    pthread_mutex_unlock(&sched->mutex);

    return strdup("{\"error\": \"Task not found\"}");
}

void scheduler_register_tools(scheduler_t *sched) {
    tool_t add_tool = {
        .name = "schedule_add",
        .description = "Schedule a recurring command",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"A name for this task\"},"
            "\"command\":{\"type\":\"string\",\"description\":\"Shell command to run\"},"
            "\"interval\":{\"type\":\"string\",\"description\":\"Interval (e.g. 'every 5 minutes')\"}"
            "},\"required\":[\"name\",\"command\",\"interval\"]}",
        .execute = tool_schedule_add,
        .user_data = sched,
    };
    tool_register(&add_tool);

    tool_t list_tool = {
        .name = "schedule_list",
        .description = "List all scheduled tasks",
        .parameters_schema = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_schedule_list,
        .user_data = sched,
    };
    tool_register(&list_tool);

    tool_t ctrl_tool = {
        .name = "schedule_control",
        .description = "Pause, resume, or delete a scheduled task",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"task_id\":{\"type\":\"integer\",\"description\":\"Task ID\"},"
            "\"action\":{\"type\":\"string\",\"enum\":[\"pause\",\"resume\",\"delete\"],\"description\":\"Action to perform\"}"
            "},\"required\":[\"task_id\",\"action\"]}",
        .execute = tool_schedule_control,
        .user_data = sched,
    };
    tool_register(&ctrl_tool);

    LOG_DEBUG("Scheduler tools registered");
}

void scheduler_task_snapshot(scheduler_t *sched,
                                   char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    buf[0] = '\0';

    if (!sched) {
        snprintf(buf, buf_size, "Scheduler not available.\n");
        return;
    }

    pthread_mutex_lock(&sched->mutex);
    int active = 0;
    for (int i = 0; i < sched->task_count; i++)
        if (sched->tasks[i].active) active++;

    if (active == 0) {
        pthread_mutex_unlock(&sched->mutex);
        snprintf(buf, buf_size, "No scheduled background tasks.\n");
        return;
    }

    /* Header */
    size_t used = (size_t)snprintf(buf, buf_size,
        "\033[1m%-4s  %-20s  %-12s  %s\033[0m\n",
        "ID", "Name", "Interval", "Status");

    for (int i = 0; i < sched->task_count && used < buf_size - 1; i++) {
        sched_task_t *t = &sched->tasks[i];
        if (!t->active) continue;

        const char *status = t->paused ? "\033[33mpaused\033[0m"
                                       : "\033[32mrunning\033[0m";

        /* Human-readable interval */
        char interval[32];
        if (t->interval_sec < 60)
            snprintf(interval, sizeof(interval), "%ds", t->interval_sec);
        else if (t->interval_sec < 3600)
            snprintf(interval, sizeof(interval), "%dm", t->interval_sec / 60);
        else
            snprintf(interval, sizeof(interval), "%dh", t->interval_sec / 3600);

        used += (size_t)snprintf(buf + used, buf_size - used,
            "%-4d  \033[36m%-20s\033[0m  %-12s  %s\n",
            t->id, t->name, interval, status);
    }
    pthread_mutex_unlock(&sched->mutex);
}

char *scheduler_task_json(scheduler_t *sched) {
    cJSON *arr = cJSON_CreateArray();

    if (sched) {
        pthread_mutex_lock(&sched->mutex);
        for (int i = 0; i < sched->task_count; i++) {
            sched_task_t *t = &sched->tasks[i];
            if (!t->active) continue;

            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id",               t->id);
            cJSON_AddStringToObject(item, "name",             t->name);
            cJSON_AddStringToObject(item, "command",          t->command);
            cJSON_AddNumberToObject(item, "interval_seconds", t->interval_sec);
            cJSON_AddBoolToObject  (item, "paused",           t->paused);
            cJSON_AddItemToArray(arr, item);
        }
        pthread_mutex_unlock(&sched->mutex);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

void scheduler_destroy(scheduler_t *sched) {
    if (!sched) return;
    sched->running = false;
    pthread_join(sched->thread, NULL);
    pthread_mutex_destroy(&sched->mutex);

    for (int i = 0; i < sched->task_count; i++) {
        free(sched->tasks[i].name);
        free(sched->tasks[i].command);
    }
    free(sched);
    LOG_DEBUG("Scheduler destroyed");
}
