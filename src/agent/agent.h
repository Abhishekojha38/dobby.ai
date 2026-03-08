/*
 * agent.h — Core agent with agentic tool loop
 */
#ifndef AGENT_H
#define AGENT_H

#include "../core/dobby.h"
#include "../core/cJSON.h"
#include "../providers/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_MESSAGES     128  /* hard array limit; runtime limit via agent->max_messages */
#define MAX_ITERATIONS   10
#define MAX_SYSTEM_PARTS 16

typedef struct {
    char *role;      /* "system", "user", "assistant", "tool" */
    char *content;
    char *tool_call_id;  /* For tool results */
    char *name;          /* Tool name for tool results */

    /* Tool calls made by assistant */
    cJSON *tool_calls;   /* Array of tool call objects */
} message_t;

typedef struct agent {
    provider_t  *provider;
    message_t    messages[MAX_MESSAGES];
    int                message_count;
    int                max_iterations;
    /* Runtime history limit: compact when message_count reaches this.
     * 0 means use MAX_MESSAGES (the hard array limit).
     * Set via dobby.conf [agent] max_messages or MAX_MESSAGES env.
     * Tune down for low-TPM models; leave at 0 for unlimited models. */
    int                max_messages;

    /* System prompt parts — composed from skills, memory, etc. */
    char              *system_parts[MAX_SYSTEM_PARTS];
    int                system_part_count;

    /* Callbacks */
    void (*on_thinking)(const char *status, void *user_data);
    void (*on_response)(const char *text, void *user_data);
    void *callback_data;
} agent_t;

/* Create and destroy agent. */
agent_t *agent_create(provider_t *provider);
void           agent_destroy(agent_t *agent);

/* Add a system prompt part (composed into final system message). */
void agent_add_system_part(agent_t *agent, const char *part);

/* Run the agentic loop: send user message, iterate tool calls, return final response.
 * Returns heap-allocated response string (caller frees). */
char *agent_chat(agent_t *agent, const char *user_message);

/* Start a new conversation (clear history, keep system prompt). */
void agent_new_conversation(agent_t *agent);

/* Get conversation statistics. */
int agent_message_count(const agent_t *agent);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_H */
