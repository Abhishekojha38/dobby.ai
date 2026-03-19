/*
 * subagent.c — Parallel sub-agent pool
 *
 * Design notes
 * ────────────
 * One mutex guards the entire slot table.  Tool calls (spawn /
 * status) hold it only to read/write metadata — never while calling
 * the LLM.  The worker thread holds it only for the brief state
 * transition at the start and end of agent_chat().
 *
 * pthread_cancel(PTHREAD_CANCEL_DEFERRED) is used for timeout.
 * The cancellation point fires inside curl_easy_perform() (the HTTP
 * read) so the provider call is interrupted cleanly.
 *
 * The watchdog is a detached thread that sleeps for timeout_sec then
 * cancels the worker if it is still running.  The watchdog frees its
 * own args struct before returning.
 */
#define _GNU_SOURCE
#include "subagent.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Slot ───────────────────────────────────────────────────────── */

typedef struct {
    int               id;
    subagent_state_t  state;

    char             *name;       /* short label */
    char             *task;       /* natural-language instruction */
    char             *context;    /* optional context from orchestrator */
    char             *result;     /* final response, error, or timeout msg */

    time_t            started_at;
    time_t            finished_at;
    int               timeout_sec;

    agent_t          *agent;       /* owned by this slot */
    pthread_t         thread;
    bool              thread_valid; /* pthread_create succeeded */

    subagent_pool_t  *pool;        /* back-pointer */
} slot_t;

/* ── Pool ───────────────────────────────────────────────────────── */

struct subagent_pool {
    slot_t           slots[SUBAGENT_MAX_SLOTS];
    int              next_id;
    pthread_mutex_t  lock;
    agent_t         *tmpl;       /* template agent — borrowed */
    provider_t      *provider;
    int              timeout_sec;
};

/* ── Helpers ────────────────────────────────────────────────────── */

static const char *state_str(subagent_state_t s)
{
    switch (s) {
        case SA_IDLE:    return "idle";
        case SA_RUNNING: return "running";
        case SA_DONE:    return "done";
        case SA_ERROR:   return "error";
        case SA_TIMEOUT: return "timeout";
        default:         return "unknown";
    }
}

static void slot_set_result(slot_t *sl, const char *text)
{
    free(sl->result);
    if (!text) { sl->result = strdup("(no result)"); return; }

    size_t len = strlen(text);
    if (len > SUBAGENT_RESULT_CAP) {
        sl->result = malloc(SUBAGENT_RESULT_CAP + 64);
        if (sl->result) {
            memcpy(sl->result, text, SUBAGENT_RESULT_CAP);
            snprintf(sl->result + SUBAGENT_RESULT_CAP, 64,
                     "\n...[truncated at %d bytes]", SUBAGENT_RESULT_CAP);
        }
    } else {
        sl->result = strdup(text);
    }
}

/* Build a fresh agent_t for a slot, cloning the template */
static agent_t *make_agent(subagent_pool_t *pool, slot_t *sl)
{
    agent_t *a = agent_create(pool->provider);
    if (!a) return NULL;

    /* Clone system parts from template */
    if (pool->tmpl) {
        for (int i = 0; i < pool->tmpl->system_part_count; i++) {
            if (pool->tmpl->system_parts[i])
                agent_add_system_part(a, pool->tmpl->system_parts[i]);
        }
        a->max_iterations = pool->tmpl->max_iterations;
        a->max_messages   = pool->tmpl->max_messages;
    }

    /* Identity — tells the sub-agent its role and constraints */
    char id_block[512];
    snprintf(id_block, sizeof(id_block),
        "## Subagent Identity\n"
        "You are subagent #%d (\"%s\"), a focused parallel worker.\n"
        "Complete only the task given. Do not ask clarifying questions —\n"
        "use your best judgment and tools. Your final response is returned\n"
        "to the orchestrator agent that spawned you.\n",
        sl->id, sl->name ? sl->name : "worker");
    agent_add_system_part(a, id_block);

    /* Optional context from orchestrator */
    if (sl->context && *sl->context) {
        char ctx_block[8192];
        snprintf(ctx_block, sizeof(ctx_block),
            "## Context from Orchestrator\n%s", sl->context);
        agent_add_system_part(a, ctx_block);
    }

    return a;
}

/* Reset a slot to idle, freeing all owned resources.
 * Caller must hold pool->lock and must have already joined the thread. */
static void slot_reset(slot_t *sl)
{
    if (sl->agent) { agent_destroy(sl->agent); sl->agent = NULL; }
    free(sl->name);    sl->name    = NULL;
    free(sl->task);    sl->task    = NULL;
    free(sl->context); sl->context = NULL;
    free(sl->result);  sl->result  = NULL;
    sl->thread_valid = false;
    sl->state        = SA_IDLE;
}

/* ── Worker thread ──────────────────────────────────────────────── */

static void *worker_fn(void *arg)
{
    slot_t *sl = (slot_t *)arg;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE,  NULL);
    pthread_setcanceltype (PTHREAD_CANCEL_DEFERRED, NULL);

    LOG_INFO("Subagent #%d (%s) started: %.120s",
             sl->id, sl->name ? sl->name : "?", sl->task ? sl->task : "");

    char *response = agent_chat(sl->agent, sl->task);

    pthread_mutex_lock(&sl->pool->lock);
    sl->finished_at = time(NULL);
    /* agent_chat returns an error string starting with "[Error" or
     * "[Dobby:" on fatal failure — treat those as errors */
    if (response &&
        (strncmp(response, "[Error",  6) == 0 ||
         strncmp(response, "[Dobby:", 7) == 0)) {
        sl->state = SA_ERROR;
    } else {
        sl->state = SA_DONE;
    }
    slot_set_result(sl, response);
    free(response);

    LOG_INFO("Subagent #%d (%s) %s (%.1f s)",
             sl->id, sl->name ? sl->name : "?",
             state_str(sl->state),
             difftime(sl->finished_at, sl->started_at));

    pthread_mutex_unlock(&sl->pool->lock);
    return NULL;
}

/* ── Watchdog thread ────────────────────────────────────────────── */

typedef struct {
    subagent_pool_t *pool;
    int              id;
    int              timeout_sec;
} watchdog_args_t;

static void *watchdog_fn(void *arg)
{
    watchdog_args_t *wa = (watchdog_args_t *)arg;
    sleep((unsigned int)wa->timeout_sec);

    pthread_mutex_lock(&wa->pool->lock);
    for (int i = 0; i < SUBAGENT_MAX_SLOTS; i++) {
        slot_t *sl = &wa->pool->slots[i];
        if (sl->id == wa->id && sl->state == SA_RUNNING && sl->thread_valid) {
            LOG_WARN("Subagent #%d timed out after %d s — cancelling",
                     wa->id, wa->timeout_sec);
            pthread_cancel(sl->thread);
            sl->state       = SA_TIMEOUT;
            sl->finished_at = time(NULL);
            slot_set_result(sl, "[Subagent timed out]");
            break;
        }
    }
    pthread_mutex_unlock(&wa->pool->lock);
    free(wa);
    return NULL;
}

/* ── Slot allocation ────────────────────────────────────────────── */

/*
 * Find a free slot or evict the oldest completed slot.
 * Must be called with pool->lock held.
 * Returns slot index, or -1 if all slots are actively running.
 */
static int alloc_slot(subagent_pool_t *pool)
{
    /* First pass: genuinely idle */
    for (int i = 0; i < SUBAGENT_MAX_SLOTS; i++)
        if (pool->slots[i].state == SA_IDLE) return i;

    /* Second pass: evict oldest completed */
    int   best_idx  = -1;
    time_t best_time = time(NULL) + 1;
    for (int i = 0; i < SUBAGENT_MAX_SLOTS; i++) {
        subagent_state_t s = pool->slots[i].state;
        if (s == SA_DONE || s == SA_ERROR || s == SA_TIMEOUT) {
            if (pool->slots[i].finished_at < best_time) {
                best_time = pool->slots[i].finished_at;
                best_idx  = i;
            }
        }
    }
    if (best_idx >= 0) {
        slot_t *sl = &pool->slots[best_idx];
        if (sl->thread_valid) {
            pthread_join(sl->thread, NULL);
        }
        slot_reset(sl);
    }
    return best_idx;
}

/* ── Tool: spawn_subagent ───────────────────────────────────────── */

static char *tool_spawn(const cJSON *args, void *user_data)
{
    subagent_pool_t *pool    = (subagent_pool_t *)user_data;
    const char      *name    = cJSON_GetStringValue(cJSON_GetObjectItem(args, "name"));
    const char      *task    = cJSON_GetStringValue(cJSON_GetObjectItem(args, "task"));
    const char      *context = cJSON_GetStringValue(cJSON_GetObjectItem(args, "context"));

    if (!task || !*task)
        return strdup("{\"error\":\"'task' is required\"}");
    if (!name || !*name) name = "worker";

    pthread_mutex_lock(&pool->lock);

    int idx = alloc_slot(pool);
    if (idx < 0) {
        pthread_mutex_unlock(&pool->lock);
        return strdup("{\"error\":\"All subagent slots are busy with running tasks. "
                      "Use subagent_status to check for completions.\"}");
    }

    slot_t *sl      = &pool->slots[idx];
    sl->id          = pool->next_id++;
    sl->state       = SA_RUNNING;
    sl->name        = strdup(name);
    sl->task        = strdup(task);
    sl->context     = context ? strdup(context) : NULL;
    sl->started_at  = time(NULL);
    sl->timeout_sec = pool->timeout_sec;
    sl->pool        = pool;
    sl->result      = NULL;
    sl->thread_valid = false;

    sl->agent = make_agent(pool, sl);
    if (!sl->agent) {
        slot_reset(sl);
        pthread_mutex_unlock(&pool->lock);
        return strdup("{\"error\":\"Failed to create sub-agent (OOM)\"}");
    }

    int  new_id  = sl->id;
    int  timeout = sl->timeout_sec;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int rc = pthread_create(&sl->thread, &attr, worker_fn, sl);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        slot_reset(sl);
        pthread_mutex_unlock(&pool->lock);
        return strdup("{\"error\":\"pthread_create failed\"}");
    }
    sl->thread_valid = true;

    pthread_mutex_unlock(&pool->lock);

    /* Spawn detached watchdog */
    watchdog_args_t *wa = malloc(sizeof(*wa));
    if (wa) {
        wa->pool        = pool;
        wa->id          = new_id;
        wa->timeout_sec = timeout;
        pthread_t wt;
        pthread_attr_t wat;
        pthread_attr_init(&wat);
        pthread_attr_setdetachstate(&wat, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&wt, &wat, watchdog_fn, wa) != 0)
            free(wa);
        pthread_attr_destroy(&wat);
    }

    LOG_INFO("Spawned subagent #%d (%s)", new_id, name);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "subagent_id", new_id);
    cJSON_AddStringToObject(out, "name",   name);
    cJSON_AddStringToObject(out, "status", "running");
    cJSON_AddStringToObject(out, "note",
        "Sub-agent is running in parallel. "
        "Use subagent_status with this subagent_id to poll for the result. "
        "You may continue other work in the meantime.");
    char *json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return json;
}

/* ── Tool: subagent_status ──────────────────────────────────────── */

static char *tool_status(const cJSON *args, void *user_data)
{
    subagent_pool_t *pool  = (subagent_pool_t *)user_data;
    cJSON           *id_j  = cJSON_GetObjectItem(args, "subagent_id");

    pthread_mutex_lock(&pool->lock);

    /* No id → list all non-idle slots */
    if (!id_j || !cJSON_IsNumber(id_j)) {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < SUBAGENT_MAX_SLOTS; i++) {
            slot_t *sl = &pool->slots[i];
            if (sl->state == SA_IDLE) continue;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "subagent_id", sl->id);
            cJSON_AddStringToObject(item, "name",   sl->name ? sl->name : "");
            cJSON_AddStringToObject(item, "status", state_str(sl->state));
            cJSON_AddStringToObject(item, "task",   sl->task ? sl->task : "");
            cJSON_AddNumberToObject(item, "elapsed_sec",
                difftime(time(NULL), sl->started_at));
            if (sl->state != SA_RUNNING && sl->result)
                cJSON_AddStringToObject(item, "result", sl->result);
            cJSON_AddItemToArray(arr, item);
        }
        pthread_mutex_unlock(&pool->lock);
        char *json = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        return json ? json : strdup("[]");
    }

    int target = id_j->valueint;

    for (int i = 0; i < SUBAGENT_MAX_SLOTS; i++) {
        slot_t *sl = &pool->slots[i];
        if (sl->id != target || sl->state == SA_IDLE) continue;

        cJSON *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(out, "subagent_id", sl->id);
        cJSON_AddStringToObject(out, "name",   sl->name ? sl->name : "");
        cJSON_AddStringToObject(out, "status", state_str(sl->state));
        cJSON_AddStringToObject(out, "task",   sl->task ? sl->task : "");
        cJSON_AddNumberToObject(out, "elapsed_sec",
            difftime(time(NULL), sl->started_at));

        if (sl->state == SA_RUNNING) {
            cJSON_AddStringToObject(out, "note",
                "Still running. Call subagent_status again later to check.");
        } else {
            cJSON_AddStringToObject(out, "result",
                sl->result ? sl->result : "(no result)");
            cJSON_AddNumberToObject(out, "duration_sec",
                difftime(sl->finished_at, sl->started_at));
        }

        pthread_mutex_unlock(&pool->lock);
        char *json = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
        return json;
    }

    pthread_mutex_unlock(&pool->lock);
    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"error\":\"Subagent #%d not found or already recycled\"}", target);
    return strdup(buf);
}

/* ── Public API ─────────────────────────────────────────────────── */

subagent_pool_t *subagent_pool_create(agent_t    *template_agent,
                                       provider_t *provider,
                                       int         timeout_sec)
{
    if (!provider) {
        LOG_ERROR("subagent_pool_create: provider required");
        return NULL;
    }
    subagent_pool_t *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->tmpl        = template_agent;
    pool->provider    = provider;
    pool->timeout_sec = timeout_sec > 0 ? timeout_sec : SUBAGENT_DEFAULT_TTL;
    pool->next_id     = 1;
    pthread_mutex_init(&pool->lock, NULL);

    LOG_DEBUG("Subagent pool created (slots=%d timeout=%ds)",
              SUBAGENT_MAX_SLOTS, pool->timeout_sec);
    return pool;
}

void subagent_register_tools(subagent_pool_t *pool)
{
    if (!pool) return;

    tool_t spawn_tool = {
        .name        = "spawn_subagent",
        .description =
            "Spawn an independent sub-agent to work on a task in parallel. "
            "The sub-agent runs in its own thread with full tool access "
            "(shell_exec, file_read, file_write, send_email, memory_store, …) "
            "and its own isolated context window. "
            "Returns a subagent_id immediately — you are NOT blocked. "
            "Best for: long-running jobs, parallel research, concurrent file work, "
            "or any self-contained delegated task. "
            "After spawning, continue other work then check results with subagent_status.",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\","
              "\"description\":\"Short label, e.g. \\\"disk-report\\\" or \\\"email-draft\\\"\"},"
            "\"task\":{\"type\":\"string\","
              "\"description\":\"Complete natural-language instruction. "
              "Be specific — the sub-agent cannot ask follow-up questions. "
              "Example: \\\"Check disk usage on all mounted volumes and write "
              "a summary to workspace/disk-report.md\\\"\"},"
            "\"context\":{\"type\":\"string\","
              "\"description\":\"Optional context to share: user preferences, "
              "prior results, relevant file paths, constraints.\"}"
            "},\"required\":[\"name\",\"task\"]}",
        .execute   = tool_spawn,
        .user_data = pool,
    };
    tool_register(&spawn_tool);

    tool_t status_tool = {
        .name        = "subagent_status",
        .description =
            "Check status and result of a sub-agent. "
            "Pass subagent_id (from spawn_subagent) for a specific agent, "
            "or omit it to list all active sub-agents. "
            "Status: 'running' = still working; 'done' = result ready; "
            "'error' = failed; 'timeout' = exceeded time limit.",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"subagent_id\":{\"type\":\"integer\","
              "\"description\":\"ID from spawn_subagent. Omit to list all sub-agents.\"}"
            "},\"required\":[]}",
        .execute   = tool_status,
        .user_data = pool,
    };
    tool_register(&status_tool);

    LOG_DEBUG("Subagent tools registered (spawn_subagent, subagent_status)");
}

char *subagent_pool_json(subagent_pool_t *pool)
{
    cJSON *arr = cJSON_CreateArray();

    if (pool) {
        pthread_mutex_lock(&pool->lock);
        for (int i = 0; i < SUBAGENT_MAX_SLOTS; i++) {
            slot_t *sl = &pool->slots[i];
            if (sl->state == SA_IDLE) continue;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id",     sl->id);
            cJSON_AddStringToObject(item, "name",   sl->name ? sl->name : "");
            cJSON_AddStringToObject(item, "status", state_str(sl->state));
            cJSON_AddStringToObject(item, "task",   sl->task ? sl->task : "");
            cJSON_AddNumberToObject(item, "elapsed_sec",
                difftime(time(NULL), sl->started_at));
            if (sl->state != SA_RUNNING && sl->result)
                cJSON_AddStringToObject(item, "result", sl->result);
            cJSON_AddItemToArray(arr, item);
        }
        pthread_mutex_unlock(&pool->lock);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json ? json : strdup("[]");
}

void subagent_pool_destroy(subagent_pool_t *pool)
{
    if (!pool) return;

    /* Signal cancellation to all running workers */
    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < SUBAGENT_MAX_SLOTS; i++) {
        slot_t *sl = &pool->slots[i];
        if (sl->state == SA_RUNNING && sl->thread_valid) {
            LOG_DEBUG("Subagent pool destroy: cancelling #%d", sl->id);
            pthread_cancel(sl->thread);
        }
    }
    pthread_mutex_unlock(&pool->lock);

    /* Join all started threads — must be done outside the lock */
    for (int i = 0; i < SUBAGENT_MAX_SLOTS; i++) {
        slot_t *sl = &pool->slots[i];
        if (sl->thread_valid) {
            pthread_join(sl->thread, NULL);
            sl->thread_valid = false;
        }
        if (sl->agent) { agent_destroy(sl->agent); sl->agent = NULL; }
        free(sl->name);    sl->name    = NULL;
        free(sl->task);    sl->task    = NULL;
        free(sl->context); sl->context = NULL;
        free(sl->result);  sl->result  = NULL;
    }

    pthread_mutex_destroy(&pool->lock);
    free(pool);
    LOG_DEBUG("Subagent pool destroyed");
}
