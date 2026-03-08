/*
 * provider.h — LLM Provider interface (hot-swappable)
 */
#ifndef PROVIDER_H
#define PROVIDER_H

#include "../core/dobby.h"
#include "../core/cJSON.h"

#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Message format capability flags
 *
 * These flags tell build_messages_json() exactly what wire format
 * each provider expects.  Every quirk has one flag here — no
 * provider-name checks anywhere else in the codebase.
 *
 * Set these in each provider's constructor.  Defaults (all-zero)
 * produce the baseline OpenAI Chat Completions format.
 *
 * HOW TO ADD A NEW QUIRK:
 *   1. Add a bool field with a descriptive name below.
 *   2. Handle it in build_messages_json() in agent.c.
 *   3. Set it to true in the provider(s) that need it.
 *   That's it — no other files change.
 */
typedef struct {
    /* Tool result messages (role="tool"):
     *   OpenAI spec: only tool_call_id + content required.
     *   Ollama:      also requires "name" to correlate the result.
     *   Anthropic:   rejects "name" (use tool_use_id instead — future).
     * Default false = omit name (safe for OpenAI/Groq/OpenRouter). */
    bool tool_result_include_name;

    /* Assistant messages that only contain tool_calls (no text content):
     *   OpenAI/Groq/Ollama: omit "content" field entirely — safest.
     *   Some providers may want content:"" instead — set this flag.
     * Default false = omit content when empty on tool-call-only messages. */
    bool assistant_tool_call_empty_content;

    /* Whether to send tool_choice:"auto" in the request when tools present.
     *   OpenAI/Groq/Mistral: explicit "auto" improves reliability.
     *   Ollama: does not support tool_choice field at all — rejects request.
     * Default false = do not send tool_choice. */
    bool send_tool_choice_auto;

    /* Whether the provider uses JSON objects for tool call arguments
     * (true) or JSON-encoded strings (false). Applies in BOTH directions:
     *
     *   Response parsing (ollama.c):
     *     true  → Ollama returns an object; stringify before storing.
     *     false → OpenAI/Groq return a string; store as-is.
     *
     *   Request building (agent.c build_messages_json):
     *     true  → Ollama expects an object in the request body; decode
     *             stored string back to object before sending.
     *     false → OpenAI/Groq expect a string; send stored string as-is.
     *
     * Default false = string format (OpenAI wire format). */
    bool tool_args_as_object;

} msg_format_t;

/* Response from an LLM call */
typedef struct {
    char  *content;      /* Text response (may be NULL if tool_calls) */
    cJSON *tool_calls;   /* Array of tool call objects (may be NULL) */
    char  *error;        /* Error message (NULL on success) */
    int    prompt_tokens;
    int    completion_tokens;
} llm_response_t;

/* Provider interface — each LLM backend implements these */
typedef struct provider {
    const char        *name;
    const char        *model;
    msg_format_t fmt;   /* wire format capabilities — set at construction */

    /* Send chat completion request.
     * messages_json: JSON string of messages array
     * tools_json:    JSON string of tools array (may be NULL)
     * Returns response struct.  */
    llm_response_t (*chat_complete)(struct provider *self,
                                          const char *messages_json,
                                          const char *tools_json);

    /* Cleanup */
    void (*destroy)(struct provider *self);

    /* Private provider data */
    void *priv;
} provider_t;

/* Free response fields */
static inline void llm_response_free(llm_response_t *r) {
    if (r->content)    { free(r->content);    r->content = NULL; }
    if (r->error)      { free(r->error);      r->error = NULL; }
    if (r->tool_calls) { cJSON_Delete(r->tool_calls); r->tool_calls = NULL; }
}

#ifdef __cplusplus
}
#endif

#endif /* PROVIDER_H */
