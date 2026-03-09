/*
 * scheduler.c — Cron-style task scheduler
 *
 * Two task types:
 *
 *   shell      — fork/exec a shell command (for system tasks like backups,
 *                log rotation, etc. — anything that doesn't need Dobby tools)
 *
 *   agent_turn — post a natural-language message to the agent via the bus.
 *                The agent processes it as a normal conversation turn and can
 *                call any tool: send_email, memory_store, file_write, etc.
 *                This is the correct type for "send an email every hour" or
 *                "check disk space and notify me".
 *
 * The agent_turn type solves the fundamental problem that shell commands
 * cannot call Dobby tools — tools are in-process functions, not binaries.
 *
 * Tool schema for schedule_add:
 *   type    : "shell" | "agent_turn"  (default: "agent_turn")
 *   name    : short label
 *   task    : shell command (if type=shell) OR natural language instruction
 *             (if type=agent_turn, e.g. "send a hello email to user@example.com")
 *   interval: "every N seconds/minutes/hours/days"
 */
#include "scheduler.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"
#include "../../bus/bus.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_TASKS 64

typedef enum {
    TASK_SHELL,       /* fork/exec a shell command */
    TASK_AGENT_TURN   /* inject message into agent via bus */
} task_type_t;

typedef struct {
    int          id;
    char        *name;
    task_type_t  type;
    char        *task;          /* shell command OR agent message */
    int          interval_sec;
    time_t       last_run;
    bool         paused;
    bool         active;
} sched_task_t;

struct scheduler {
    sched_task_t     tasks[MAX_TASKS];
    int              task_count;
    int              next_id;
    pthread_t        thread;
    bool             running;
    pthread_mutex_t  mutex;
    bus_t           *bus;            /* NULL = no agent tasks */
    char             jobs_path[512]; /* path to jobs.json, empty = no persistence */
};

/* ── Shell execution (fire-and-forget) ─────────────────────────── */

static void scheduler_run_async(const char *command) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_WARN("Scheduler fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        pid_t gc = fork();
        if (gc < 0) { _exit(1); }
        if (gc > 0) { _exit(0); }

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

/* ── Agent task injection ───────────────────────────────────────── */

static void scheduler_run_agent_turn(scheduler_t *sched, sched_task_t *task) {
    if (!sched->bus) {
        LOG_WARN("Scheduler: no bus — cannot run agent task '%s'", task->name);
        return;
    }

    /* Build a unique chat_id so each task gets its own session */
    char chat_id[64];
    snprintf(chat_id, sizeof(chat_id), "scheduler:%d", task->id);

    inbound_msg_t *msg = inbound_msg_new(
        "scheduler",          /* channel */
        "scheduler",          /* sender_id */
        chat_id,              /* chat_id — per-task session */
        task->task,           /* content — natural language instruction */
        NULL                  /* metadata */
    );

    if (!msg) {
        LOG_WARN("Scheduler: failed to allocate message for task '%s'", task->name);
        return;
    }

    LOG_DEBUG("Scheduler: injecting agent task '%s': %s", task->name, task->task);

    if (!bus_publish_inbound(sched->bus, msg)) {
        LOG_WARN("Scheduler: bus rejected message for task '%s'", task->name);
    }
}

/* ── Background thread ─────────────────────────────────────────── */

static void *scheduler_thread(void *arg) {
    scheduler_t *sched = (scheduler_t *)arg;

    while (sched->running) {
        sleep(1);
        time_t now = time(NULL);

        pthread_mutex_lock(&sched->mutex);
        for (int i = 0; i < sched->task_count; i++) {
            sched_task_t *task = &sched->tasks[i];
            if (!task->active || task->paused) continue;

            if (now - task->last_run >= task->interval_sec) {
                task->last_run = now;
                LOG_DEBUG("Scheduler: firing task '%s' (id=%d, type=%s)",
                          task->name, task->id,
                          task->type == TASK_SHELL ? "shell" : "agent_turn");

                if (task->type == TASK_SHELL) {
                    scheduler_run_async(task->task);
                } else {
                    /* Release lock while injecting — bus_publish_inbound may block */
                    char *task_copy = strdup(task->task);
                    char  chat_id[64];
                    snprintf(chat_id, sizeof(chat_id), "scheduler:%d", task->id);
                    int   id_copy   = task->id;
                    char *name_copy = strdup(task->name);
                    pthread_mutex_unlock(&sched->mutex);

                    if (sched->bus && task_copy) {
                        inbound_msg_t *msg = inbound_msg_new(
                            "scheduler", "scheduler", chat_id, task_copy, NULL);
                        if (msg && !bus_publish_inbound(sched->bus, msg)) {
                            LOG_WARN("Scheduler: bus rejected agent task '%s'", name_copy);
                        }
                    }
                    free(task_copy);
                    free(name_copy);
                    (void)id_copy;

                    pthread_mutex_lock(&sched->mutex);
                }
            }
        }
        pthread_mutex_unlock(&sched->mutex);
    }

    return NULL;
}

/* ── Interval parser ───────────────────────────────────────────── */

static int parse_interval(const char *spec) {
    int n = 0;
    char unit[32] = {0};

    if (sscanf(spec, "every %d %31s", &n, unit) == 2) {
        if (strstr(unit, "sec"))  return n;
        if (strstr(unit, "min"))  return n * 60;
        if (strstr(unit, "hour")) return n * 3600;
        if (strstr(unit, "day"))  return n * 86400;
    }
    if (sscanf(spec, "%d", &n) == 1 && n > 0) return n;
    return -1;
}

/* ── Persistence ───────────────────────────────────────────────── */

/* Called with mutex held. Writes all active tasks to jobs.json. */
static void save_jobs(scheduler_t *sched) {
    if (!sched->jobs_path[0]) return;

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < sched->task_count; i++) {
        sched_task_t *t = &sched->tasks[i];
        if (!t->active) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id",           t->id);
        cJSON_AddStringToObject(item, "name",         t->name);
        cJSON_AddStringToObject(item, "type",         t->type == TASK_SHELL ? "shell" : "agent_turn");
        cJSON_AddStringToObject(item, "task",         t->task);
        cJSON_AddNumberToObject(item, "interval_sec", t->interval_sec);
        cJSON_AddBoolToObject  (item, "paused",       t->paused);
        cJSON_AddItemToArray(arr, item);
    }

    char *json = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (!json) return;

    FILE *f = fopen(sched->jobs_path, "w");
    if (f) { fputs(json, f); fclose(f); }
    else LOG_WARN("Scheduler: cannot write %s", sched->jobs_path);
    free(json);
}

/* Called once at startup (before thread starts — no lock needed). */
static void load_jobs(scheduler_t *sched) {
    if (!sched->jobs_path[0]) return;

    FILE *f = fopen(sched->jobs_path, "r");
    if (!f) return;  /* no file yet — fresh start */

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr) { LOG_WARN("Scheduler: bad JSON in %s", sched->jobs_path); return; }

    time_t now = time(NULL);
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (sched->task_count >= MAX_TASKS) break;

        const char *name     = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        const char *type_str = cJSON_GetStringValue(cJSON_GetObjectItem(item, "type"));
        const char *task     = cJSON_GetStringValue(cJSON_GetObjectItem(item, "task"));
        cJSON      *iv       = cJSON_GetObjectItem(item, "interval_sec");
        cJSON      *id_j     = cJSON_GetObjectItem(item, "id");
        cJSON      *paused_j = cJSON_GetObjectItem(item, "paused");

        if (!name || !task || !iv) continue;

        sched_task_t *t  = &sched->tasks[sched->task_count++];
        t->id            = id_j ? id_j->valueint : sched->next_id;
        t->name          = strdup(name);
        t->type          = (type_str && strcmp(type_str, "shell") == 0) ? TASK_SHELL : TASK_AGENT_TURN;
        t->task          = strdup(task);
        t->interval_sec  = iv->valueint;
        t->last_run      = now;   /* don't fire immediately on restart */
        t->paused        = paused_j && cJSON_IsTrue(paused_j);
        t->active        = true;

        if (t->id >= sched->next_id) sched->next_id = t->id + 1;
    }
    cJSON_Delete(arr);
    LOG_DEBUG("Scheduler: loaded %d tasks from %s", sched->task_count, sched->jobs_path);
}

/* ── Public API ────────────────────────────────────────────────── */

scheduler_t *scheduler_create(bus_t *bus, const char *workspace) {
    scheduler_t *sched = calloc(1, sizeof(scheduler_t));
    sched->next_id = 1;
    sched->running = true;
    sched->bus     = bus;
    pthread_mutex_init(&sched->mutex, NULL);

    /* Build jobs.json path and ensure directory exists */
    if (workspace && *workspace) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/scheduler", workspace);
        mkdir(dir, 0755);
        snprintf(sched->jobs_path, sizeof(sched->jobs_path), "%s/jobs.json", dir);
        load_jobs(sched);  /* restore persisted tasks before thread starts */
    }

    pthread_create(&sched->thread, NULL, scheduler_thread, sched);
    LOG_DEBUG("Scheduler started (%d tasks loaded)", sched->task_count);
    return sched;
}

/* ── Tool implementations ──────────────────────────────────────── */

static char *tool_schedule_add(const cJSON *args, void *user_data) {
    scheduler_t *sched    = (scheduler_t *)user_data;
    const char  *name     = cJSON_GetStringValue(cJSON_GetObjectItem(args, "name"));
    const char  *task     = cJSON_GetStringValue(cJSON_GetObjectItem(args, "task"));
    const char  *interval = cJSON_GetStringValue(cJSON_GetObjectItem(args, "interval"));
    const char  *type_str = cJSON_GetStringValue(cJSON_GetObjectItem(args, "type"));

    if (!name || !task || !interval) {
        return strdup("{\"error\": \"name, task, and interval are required\"}");
    }

    /* Default type is agent_turn — the correct choice for any Dobby tool action */
    task_type_t type = TASK_AGENT_TURN;
    if (type_str && strcmp(type_str, "shell") == 0) {
        type = TASK_SHELL;
    }

    int interval_sec = parse_interval(interval);
    if (interval_sec <= 0) {
        return strdup("{\"error\": \"Invalid interval. Use 'every N seconds/minutes/hours/days'\"}");
    }

    pthread_mutex_lock(&sched->mutex);
    if (sched->task_count >= MAX_TASKS) {
        pthread_mutex_unlock(&sched->mutex);
        return strdup("{\"error\": \"Max tasks reached\"}");
    }

    sched_task_t *t  = &sched->tasks[sched->task_count++];
    t->id            = sched->next_id++;
    t->name          = strdup(name);
    t->type          = type;
    t->task          = strdup(task);
    t->interval_sec  = interval_sec;
    t->last_run      = time(NULL);
    t->paused        = false;
    t->active        = true;
    save_jobs(sched);
    pthread_mutex_unlock(&sched->mutex);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "scheduled");
    cJSON_AddNumberToObject(result, "task_id", t->id);
    cJSON_AddStringToObject(result, "name", name);
    cJSON_AddStringToObject(result, "type", type == TASK_SHELL ? "shell" : "agent_turn");
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
        cJSON_AddStringToObject(item, "type", t->type == TASK_SHELL ? "shell" : "agent_turn");
        cJSON_AddStringToObject(item, "task", t->task);
        cJSON_AddNumberToObject(item, "interval_seconds", t->interval_sec);
        cJSON_AddBoolToObject  (item, "paused", t->paused);
        cJSON_AddItemToArray(result, item);
    }
    pthread_mutex_unlock(&sched->mutex);

    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

static char *tool_schedule_control(const cJSON *args, void *user_data) {
    scheduler_t *sched  = (scheduler_t *)user_data;
    const char  *action = cJSON_GetStringValue(cJSON_GetObjectItem(args, "action"));

    if (!action) {
        return strdup("{\"error\": \"action required\"}");
    }

    if (strcmp(action, "list") == 0) {
        return tool_schedule_list(args, user_data);
    }

    cJSON *id_val = cJSON_GetObjectItem(args, "task_id");
    if (!id_val) {
        return strdup("{\"error\": \"task_id required for pause/resume/delete\"}");
    }

    int task_id = id_val->valueint;

    pthread_mutex_lock(&sched->mutex);
    for (int i = 0; i < sched->task_count; i++) {
        sched_task_t *t = &sched->tasks[i];
        if (t->id != task_id || !t->active) continue;

        if (strcmp(action, "pause") == 0) {
            t->paused = true;
        } else if (strcmp(action, "resume") == 0) {
            t->paused = false;
        } else if (strcmp(action, "delete") == 0) {
            t->active = false;
            free(t->name);    t->name = NULL;
            free(t->task);    t->task = NULL;
        } else {
            pthread_mutex_unlock(&sched->mutex);
            return strdup("{\"error\": \"unknown action — use list/pause/resume/delete\"}");
        }
        save_jobs(sched);
        pthread_mutex_unlock(&sched->mutex);

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", action);
        cJSON_AddNumberToObject(result, "task_id", task_id);
        char *json = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);
        return json;
    }
    pthread_mutex_unlock(&sched->mutex);
    return strdup("{\"error\": \"Task not found\"}");
}

/* ── Tool registration ─────────────────────────────────────────── */

void scheduler_register_tools(scheduler_t *sched) {
    tool_t add_tool = {
        .name        = "schedule_add",
        .description =
            "Schedule a recurring task. "
            "Use type=\"agent_turn\" (default) when the task needs Dobby tools "
            "(send_email, file ops, memory, etc.) — set 'task' to a natural language "
            "instruction like \"send a status email to user@example.com\". "
            "Use type=\"shell\" only for pure system commands that need no Dobby tools.",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short label for this task\"},"
            "\"task\":{\"type\":\"string\","
              "\"description\":\"For agent_turn: natural language instruction (e.g. 'send a hello email to user@example.com'). "
              "For shell: shell command to run.\"},"
            "\"interval\":{\"type\":\"string\","
              "\"description\":\"How often to run, e.g. 'every 5 minutes', 'every 2 hours'\"},"
            "\"type\":{\"type\":\"string\",\"enum\":[\"agent_turn\",\"shell\"],"
              "\"description\":\"agent_turn (default): runs via agent loop with full tool access. shell: raw shell command.\"}"
            "},\"required\":[\"name\",\"task\",\"interval\"]}",
        .execute   = tool_schedule_add,
        .user_data = sched,
    };
    tool_register(&add_tool);

    tool_t list_tool = {
        .name        = "schedule_list",
        .description = "List all scheduled tasks with their IDs, types, intervals, and status",
        .parameters_schema = "{\"type\":\"object\",\"properties\":{}}",
        .execute   = tool_schedule_list,
        .user_data = sched,
    };
    tool_register(&list_tool);

    tool_t ctrl_tool = {
        .name        = "schedule_control",
        .description =
            "Manage scheduled tasks: list all, or pause/resume/delete by ID. "
            "Use 'list' first to see task IDs.",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"action\":{\"type\":\"string\","
              "\"enum\":[\"list\",\"pause\",\"resume\",\"delete\"],"
              "\"description\":\"list: show all tasks (no task_id needed). "
              "pause/resume/delete: require task_id.\"},"
            "\"task_id\":{\"type\":\"integer\","
              "\"description\":\"Task ID (required for pause/resume/delete)\"}"
            "},\"required\":[\"action\"]}",
        .execute   = tool_schedule_control,
        .user_data = sched,
    };
    tool_register(&ctrl_tool);

    LOG_DEBUG("Scheduler tools registered");
}

/* ── Snapshot / JSON helpers ───────────────────────────────────── */

void scheduler_task_snapshot(scheduler_t *sched, char *buf, size_t buf_size) {
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

    size_t used = (size_t)snprintf(buf, buf_size,
        "\033[1m%-4s  %-20s  %-11s  %-12s  %s\033[0m\n",
        "ID", "Name", "Type", "Interval", "Status");

    for (int i = 0; i < sched->task_count && used < buf_size - 1; i++) {
        sched_task_t *t = &sched->tasks[i];
        if (!t->active) continue;

        const char *status = t->paused ? "\033[33mpaused\033[0m" : "\033[32mrunning\033[0m";
        const char *type   = t->type == TASK_SHELL ? "shell" : "agent";

        char interval[32];
        if      (t->interval_sec < 60)   snprintf(interval, sizeof(interval), "%ds",  t->interval_sec);
        else if (t->interval_sec < 3600) snprintf(interval, sizeof(interval), "%dm",  t->interval_sec / 60);
        else                             snprintf(interval, sizeof(interval), "%dh",  t->interval_sec / 3600);

        used += (size_t)snprintf(buf + used, buf_size - used,
            "%-4d  \033[36m%-20s\033[0m  %-11s  %-12s  %s\n",
            t->id, t->name, type, interval, status);
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
            cJSON_AddNumberToObject(item, "id",              t->id);
            cJSON_AddStringToObject(item, "name",            t->name);
            cJSON_AddStringToObject(item, "type",            t->type == TASK_SHELL ? "shell" : "agent_turn");
            cJSON_AddStringToObject(item, "task",            t->task);
            cJSON_AddNumberToObject(item, "interval_seconds",t->interval_sec);
            cJSON_AddBoolToObject  (item, "paused",          t->paused);
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
        free(sched->tasks[i].task);
    }
    free(sched);
    LOG_DEBUG("Scheduler destroyed");
}
