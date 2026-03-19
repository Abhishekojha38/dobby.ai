/*
 * subagent.h — Parallel sub-agent pool
 *
 * Lets the orchestrator agent spawn isolated sub-agents that run
 * concurrently, each with their own agent_t (separate context window
 * and conversation history), sharing the same provider and tool
 * registry as the main agent.
 *
 * ── How it works ────────────────────────────────────────────────
 *
 *   Orchestrator calls  spawn_subagent(name, task, context?)
 *     → returns subagent_id immediately, does NOT block
 *
 *   Sub-agent runs agent_chat() in a dedicated pthread with full
 *   tool access (shell, file, email, memory, …).
 *
 *   Orchestrator calls  subagent_status(subagent_id)
 *     → returns {status, result} at any time
 *
 *   Status values:
 *     "running"  — still working
 *     "done"     — finished; result contains the final response
 *     "error"    — agent_chat returned an error string
 *     "timeout"  — exceeded wall-clock deadline
 *
 * ── Isolation ───────────────────────────────────────────────────
 *
 *   Each sub-agent gets:
 *     - Fresh agent_t cloned from the template (isolated history)
 *     - System prompt parts copied from template agent
 *     - Injected identity block ("You are subagent #N, do only X")
 *     - Optional context string from the orchestrator
 *     - Full access to the global tool registry
 *     - Own pthread — truly parallel execution
 *
 * ── Slot lifecycle ──────────────────────────────────────────────
 *
 *   Pool has SUBAGENT_MAX_SLOTS slots.  A new spawn evicts the
 *   oldest finished/error/timeout slot when the pool is full.
 *   Running slots are never evicted.
 *
 * ── Thread safety ───────────────────────────────────────────────
 *
 *   All public functions are thread-safe (single pool mutex).
 *   agent_chat() is not re-entrant but each sub-agent owns its own
 *   agent_t, so concurrent calls are safe.
 */
#ifndef SUBAGENT_H
#define SUBAGENT_H

#include "../../core/dobby.h"
#include "../../agent/agent.h"
#include "../../agent/tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SUBAGENT_MAX_SLOTS   8
#define SUBAGENT_DEFAULT_TTL 600          /* seconds — 10 min */
#define SUBAGENT_RESULT_CAP  (64 * 1024)  /* max bytes stored per result */

typedef enum {
    SA_IDLE    = 0,
    SA_RUNNING = 1,
    SA_DONE    = 2,
    SA_ERROR   = 3,
    SA_TIMEOUT = 4,
} subagent_state_t;

typedef struct subagent_pool subagent_pool_t;

/*
 * subagent_pool_create — allocate the pool.
 *
 *   template_agent : system-prompt parts are cloned from here into
 *                    every new sub-agent.  Borrowed reference — do
 *                    not destroy before the pool.
 *   provider       : shared LLM backend.  All built-in providers use
 *                    per-call HTTP so concurrent use is safe.
 *   timeout_sec    : per-agent wall-clock deadline; 0 → default (600 s).
 */
subagent_pool_t *subagent_pool_create(agent_t    *template_agent,
                                       provider_t *provider,
                                       int         timeout_sec);

/*
 * subagent_register_tools — register spawn_subagent and subagent_status
 * with the global tool registry.  Call once after pool creation.
 */
void subagent_register_tools(subagent_pool_t *pool);

/*
 * subagent_pool_json — snapshot for GET /api/subagents.
 * Returns heap-allocated JSON array; caller frees.
 * Returns "[]" on empty or NULL pool.
 */
char *subagent_pool_json(subagent_pool_t *pool);

/*
 * subagent_pool_destroy — cancel all running threads, join them,
 * free all resources.
 */
void subagent_pool_destroy(subagent_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* SUBAGENT_H */
