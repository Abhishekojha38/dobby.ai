/*
 * agent.c — Core agent with agentic tool loop
 *
 * Loop:
 *   1. Build messages JSON (system + history + user message).
 *   2. Call the LLM provider with available tools.
 *   3. If the LLM responds with tool_calls → execute each, append results.
 *   4. Repeat until the LLM gives a plain text response or max iterations.
 *
 * Fixes carried over from original (kept for posterity):
 *   [FIX-1] strip_think_tags: strbuf freed only after strdup — no UAF.
 *   [FIX-2] Final response returns stripped version, not raw with <think> tags.
 *   [FIX-3] History compaction never splits a tool_calls/tool-result pair.
 *   [FIX-4] Tool argument parsing handles both JSON-string and JSON-object.
 *
 * Improvements in this revision:
 *   - Removed all dead `#if 0` blocks (conversational-veto feature was
 *     disabled and added noise; can be re-added behind a feature flag).
 *   - Removed commented-out LOG_INFO calls.
 *   - Extracted build_system_prompt() duplication into the existing helper.
 *   - Added NULL guard to agent_chat() for the user_message parameter.
 */
#include "agent.h"
#include "tool_registry.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Message helpers */

static void message_free(message_t *msg) {
    free(msg->role);
    free(msg->content);
    free(msg->tool_call_id);
    free(msg->name);
    if (msg->tool_calls) cJSON_Delete(msg->tool_calls);
    memset(msg, 0, sizeof(*msg));
}

static bool message_has_tool_calls(const message_t *msg) {
    return msg->tool_calls != NULL &&
           cJSON_GetArraySize(msg->tool_calls) > 0;
}

/**
 * agent_compact_history - Remove the oldest removable messages.
 *
 * Strategy: always keep index 0 (system message).  Walk the next
 * MAX_MESSAGES/4 slots and find a safe cut point — the boundary must
 * not leave a dangling assistant/tool_calls message without its corresponding
 * tool-result messages.
 */
static void agent_compact_history(agent_t *agent) {
    LOG_WARN("Message limit reached — compacting history");

    int limit = (agent->max_messages > 0 && agent->max_messages <= MAX_MESSAGES)
                ? agent->max_messages : MAX_MESSAGES;

    const int keep         = 1;              /* preserve system message at index 0 */
    int       remove_count = limit / 4;      /* remove oldest quarter of the window */
    if (remove_count < 1) remove_count = 1;
    int       cut_end      = keep + remove_count;

    if (cut_end > agent->message_count) cut_end = agent->message_count;

    /* Walk back until the cut boundary is safe (no dangling tool_calls). */
    while (cut_end > keep + 1 &&
           message_has_tool_calls(&agent->messages[cut_end - 1])) {
        cut_end--;
    }

    int actual_remove = cut_end - keep;
    if (actual_remove <= 0) {
        LOG_WARN("Cannot safely compact: all candidates have pending tool calls");
        return;
    }

    for (int i = keep; i < keep + actual_remove; i++) {
        message_free(&agent->messages[i]);
    }

    int remaining = agent->message_count - (keep + actual_remove);
    memmove(&agent->messages[keep],
            &agent->messages[keep + actual_remove],
            (size_t)remaining * sizeof(message_t));
    agent->message_count -= actual_remove;

    LOG_DEBUG("Compacted %d messages from history", actual_remove);
}

static void agent_add_message(agent_t *agent, const char *role,
                              const char *content, const char *tool_call_id,
                              const char *name, cJSON *tool_calls) {
    int limit = (agent->max_messages > 0 && agent->max_messages <= MAX_MESSAGES)
                ? agent->max_messages : MAX_MESSAGES;
    if (agent->message_count >= limit) {
        agent_compact_history(agent);
    }

    message_t *msg = &agent->messages[agent->message_count++];
    msg->role         = strdup(role);
    msg->content      = content ? strdup(content) : strdup("");
    msg->tool_call_id = tool_call_id ? strdup(tool_call_id) : NULL;
    msg->name         = name ? strdup(name) : NULL;
    msg->tool_calls   = tool_calls ? cJSON_Duplicate(tool_calls, 1) : NULL;
}

/* System prompt */

static char *build_system_prompt(agent_t *agent) {
    strbuf_t sb;
    strbuf_init(&sb, 2048);

    for (int i = 0; i < agent->system_part_count; i++) {
        strbuf_append(&sb, agent->system_parts[i]);
        strbuf_append(&sb, "\n\n");
    }

    char *result = strdup(sb.data);
    strbuf_free(&sb);
    return result;
}

/* Messages JSON builder */

/*
 * build_messages_json — serialize conversation history to the wire format
 * the current provider expects.
 *
 * All provider-specific behaviour is driven by agent->provider->fmt flags.
 * No provider names, no special-case strings here.  To change how a
 * provider receives messages, set the right flag in its constructor.
 */
static cJSON *build_messages_json(agent_t *agent) {
    const msg_format_t *fmt = &agent->provider->fmt;
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < agent->message_count; i++) {
        message_t *m   = &agent->messages[i];
        cJSON           *msg = cJSON_CreateObject();

        cJSON_AddStringToObject(msg, "role", m->role);

        /* content field:
         * For assistant messages that only made tool calls (no text),
         * the content field should either be omitted or sent as "".
         * Omitting is safest and accepted by all known providers.
         * fmt->assistant_tool_call_empty_content = true sends "" instead. */
        bool is_assistant_tool_only = strcmp(m->role, "assistant") == 0
                                      && m->tool_calls
                                      && cJSON_GetArraySize(m->tool_calls) > 0
                                      && (!m->content || !*m->content);

        if (is_assistant_tool_only) {
            if (fmt->assistant_tool_call_empty_content)
                cJSON_AddStringToObject(msg, "content", "");
            /* else: omit content entirely */
        } else {
            cJSON_AddStringToObject(msg, "content", m->content ? m->content : "");
        }

        /* tool result fields:
         * tool_call_id is required by all providers.
         * name is required by some (Ollama) and harmless on others,
         * but rejected by Anthropic — controlled by fmt flag. */
        if (strcmp(m->role, "tool") == 0) {
            if (m->tool_call_id)
                cJSON_AddStringToObject(msg, "tool_call_id", m->tool_call_id);
            if (m->name && fmt->tool_result_include_name)
                cJSON_AddStringToObject(msg, "name", m->name);
        }

        /*  tool_calls normalization:
         * Rebuild each tool call entry with the fields all providers need:
         * id, type:"function", function.name, function.arguments (string).
         * arguments must always be a JSON-encoded string — never an object. */
        if (m->tool_calls) {
            cJSON *tc_arr = cJSON_CreateArray();
            int size = cJSON_GetArraySize(m->tool_calls);

            for (int j = 0; j < size; j++) {
                cJSON *tc   = cJSON_GetArrayItem(m->tool_calls, j);
                cJSON *func = cJSON_GetObjectItem(tc, "function");
                if (!func) continue;

                cJSON *otc   = cJSON_CreateObject();
                cJSON *ofunc = cJSON_CreateObject();

                const char *tc_id = cJSON_GetStringValue(cJSON_GetObjectItem(tc, "id"));
                if (tc_id) cJSON_AddStringToObject(otc, "id", tc_id);
                cJSON_AddStringToObject(otc, "type", "function");

                const char *fname = cJSON_GetStringValue(cJSON_GetObjectItem(func, "name"));
                if (fname) cJSON_AddStringToObject(ofunc, "name", fname);

                /* arguments: format depends on what the provider expects.
                 *
                 * We always store arguments internally as a JSON-encoded string
                 * (normalized from Ollama's object in ollama.c response parsing).
                 *
                 * On the way OUT when building the request:
                 *   fmt.tool_args_as_object = false (OpenAI/Groq):
                 *     pass the stored string through as-is.
                 *   fmt.tool_args_as_object = true (Ollama):
                 *     decode the string back to a JSON object — Ollama requires
                 *     an object in the request body, it rejects a string. */
                cJSON *args_item = cJSON_GetObjectItem(func, "arguments");
                if (fmt->tool_args_as_object) {
                    /* Provider (Ollama) wants an object — decode stored string */
                    cJSON *args_obj = NULL;
                    if (args_item && cJSON_IsString(args_item) && args_item->valuestring) {
                        args_obj = cJSON_Parse(args_item->valuestring);
                    } else if (args_item && !cJSON_IsString(args_item)) {
                        args_obj = cJSON_Duplicate(args_item, 1);
                    }
                    if (!args_obj) args_obj = cJSON_CreateObject();
                    cJSON_AddItemToObject(ofunc, "arguments", args_obj);
                } else {
                    /* Provider (OpenAI/Groq) wants a string */
                    if (args_item && cJSON_IsString(args_item)) {
                        cJSON_AddStringToObject(ofunc, "arguments", args_item->valuestring);
                    } else if (args_item) {
                        char *args_str = cJSON_PrintUnformatted(args_item);
                        cJSON_AddStringToObject(ofunc, "arguments", args_str ? args_str : "{}");
                        free(args_str);
                    } else {
                        cJSON_AddStringToObject(ofunc, "arguments", "{}");
                    }
                }

                cJSON_AddItemToObject(otc, "function", ofunc);
                cJSON_AddItemToArray(tc_arr, otc);
            }

            cJSON_AddItemToObject(msg, "tool_calls", tc_arr);
        }

        cJSON_AddItemToArray(arr, msg);
    }

    return arr;
}

/* Lifecycle */

agent_t *agent_create(provider_t *provider) {
    agent_t *agent = calloc(1, sizeof(agent_t));
    if (!agent) { LOG_ERROR("OOM creating agent"); return NULL; }
    agent->provider       = provider;
    agent->max_iterations  = MAX_ITERATIONS;
    LOG_DEBUG("Agent created");
    return agent;
}

void agent_destroy(agent_t *agent) {
    if (!agent) return;
    for (int i = 0; i < agent->message_count; i++) message_free(&agent->messages[i]);
    for (int i = 0; i < agent->system_part_count; i++) free(agent->system_parts[i]);
    free(agent);
    LOG_DEBUG("Agent destroyed");
}

void agent_add_system_part(agent_t *agent, const char *part) {
    if (!agent || !part || agent->system_part_count >= MAX_SYSTEM_PARTS) return;
    agent->system_parts[agent->system_part_count++] = strdup(part);
}

void agent_new_conversation(agent_t *agent) {
    for (int i = 0; i < agent->message_count; i++) message_free(&agent->messages[i]);
    agent->message_count = 0;
    LOG_DEBUG("New conversation started");
}

int agent_message_count(const agent_t *agent) {
    return agent ? agent->message_count : 0;
}

/* Utilities */

/* Remove and free the last message in history.
 * Used to roll back a user message on provider error so history stays
 * clean and the CLI can accept new input immediately. */
static void agent_pop_last_message(agent_t *agent) {
    if (agent->message_count <= 0) return;
    message_free(&agent->messages[--agent->message_count]);
}

/**
 * strip_think_tags - Remove <think>…</think> blocks from a string.
 *
 * Returns a newly allocated, trimmed string.  Caller must free().
 * Ownership: strbuf is freed only after strdup — no use-after-free.
 */
static char *strip_think_tags(const char *input) {
    if (!input) return NULL;

    strbuf_t sb;
    strbuf_init(&sb, strlen(input) + 1);

    const char *p = input;
    while (*p) {
        const char *start = strstr(p, "<think>");
        if (start) {
            size_t diff = (size_t)(start - p);
            if (diff > 0) {
                char *chunk = strndup(p, diff);
                strbuf_append(&sb, chunk);
                free(chunk);
            }
            const char *end = strstr(start + 7, "</think>");
            if (end) {
                p = end + 8;
            } else {
                break; /* Unclosed <think> — discard the remainder. */
            }
        } else {
            strbuf_append(&sb, p);
            break;
        }
    }

    char *result = strdup(str_trim(sb.data));
    strbuf_free(&sb);
    return result;
}

/**
 * parse_tool_arguments - Extract tool arguments into a cJSON object.
 *
 * After provider normalization, arguments is always a JSON-encoded string.
 * We still handle raw objects defensively for belt-and-suspenders safety.
 *
 * Returns a newly allocated cJSON object.  Caller must cJSON_Delete().
 * Never returns NULL.
 */
static cJSON *parse_tool_arguments(cJSON *func) {
    cJSON *args_item = cJSON_GetObjectItem(func, "arguments");
    if (!args_item) return cJSON_CreateObject();

    if (cJSON_IsString(args_item)) {
        if (!args_item->valuestring || !*args_item->valuestring ||
            strcmp(args_item->valuestring, "{}") == 0) {
            return cJSON_CreateObject();
        }
        cJSON *parsed = cJSON_Parse(args_item->valuestring);
        if (!parsed) {
            LOG_ERROR("parse_tool_arguments: invalid JSON in arguments: %.300s",
                      args_item->valuestring);
            return cJSON_CreateObject();
        }
        return parsed;
    }

    /* Raw object fallback — provider normalization should prevent this */
    LOG_WARN("parse_tool_arguments: arguments is a JSON object, not a string");
    return cJSON_Duplicate(args_item, 1);
}

/* Agentic tool loop */

char *agent_chat(agent_t *agent, const char *user_message) {
    if (!agent || !user_message || !*user_message) {
        return strdup("[Error: empty message]");
    }

    /* Ensure the system message is the first entry in history. */
    if (agent->message_count == 0) {
        char *sys = build_system_prompt(agent);
        agent_add_message(agent, "system", sys, NULL, NULL, NULL);
        free(sys);
    }

    /* Record count before this turn so we can roll back cleanly on any
     * fatal error path (provider error on iter>1, max_iterations). */
    int pre_turn_count = agent->message_count;

    agent_add_message(agent, "user", user_message, NULL, NULL, NULL);

    for (int iter = 0; iter < agent->max_iterations; iter++) {
        if (agent->on_thinking) {
            char status[64];
            snprintf(status, sizeof(status), "Thinking (iteration %d)...", iter + 1);
            agent->on_thinking(status, agent->callback_data);
        }

        cJSON *messages     = build_messages_json(agent);
        char  *messages_str = cJSON_PrintUnformatted(messages);
        cJSON_Delete(messages);

        char *tools_str = tool_generate_json();

        /* Single provider call — no retry logic.
         * On any error (rate limit, network, auth) ALL messages added this
         * turn are rolled back so history is clean for the next user input. */
        llm_response_t resp = agent->provider->chat_complete(
            agent->provider, messages_str, tools_str);

        free(messages_str);
        free(tools_str);

        if (resp.error) {
            LOG_ERROR("LLM error: %s", resp.error);
            char *err = strdup(resp.error);
            llm_response_free(&resp);
            /* Roll back ALL messages added this turn (user + any tool iterations). */
            while (agent->message_count > pre_turn_count)
                agent_pop_last_message(agent);
            return err;
        }

        /* Tool-call iteration */
        if (resp.tool_calls && cJSON_GetArraySize(resp.tool_calls) > 0) {
            char *clean = strip_think_tags(resp.content);
            agent_add_message(agent, "assistant", clean, NULL, NULL, resp.tool_calls);
            free(clean);

            int num_calls = cJSON_GetArraySize(resp.tool_calls);
            for (int t = 0; t < num_calls; t++) {
                cJSON      *tc       = cJSON_GetArrayItem(resp.tool_calls, t);
                cJSON      *func     = cJSON_GetObjectItem(tc, "function");
                const char *tool_name = cJSON_GetStringValue(
                    cJSON_GetObjectItem(func, "name"));
                const char *call_id   = cJSON_GetStringValue(
                    cJSON_GetObjectItem(tc, "id"));

                const tool_t *tool = tool_find(tool_name);
                char *result;

                if (tool) {
                    cJSON *args = parse_tool_arguments(func);
                    /* Log tool call at INFO level — visible with --debug */
                    char *args_str = cJSON_PrintUnformatted(args);
                    LOG_INFO("tool call: %s  args=%s",
                             tool_name ? tool_name : "?",
                             args_str ? args_str : "{}");
                    free(args_str);
                    result = tool->execute(args, tool->user_data);
                    cJSON_Delete(args);
                } else {
                    LOG_WARN("Unknown tool requested: %s",
                             tool_name ? tool_name : "<null>");
                    result = strdup("{\"error\": \"Unknown tool\"}");
                }

                agent_add_message(agent, "tool", result, call_id, tool_name, NULL);
                free(result);
            }

            free(resp.content);
            free(resp.error);
            cJSON_Delete(resp.tool_calls);
            resp.content = NULL; resp.error = NULL; resp.tool_calls = NULL;
            continue;
        }

        /* Final text response */
        char *clean_final = strip_think_tags(resp.content);
        llm_response_free(&resp);

        agent_add_message(agent, "assistant", clean_final, NULL, NULL, NULL);

        char *return_val = strdup(clean_final);
        free(clean_final);
        return return_val;
    }

    /* Max iterations — roll back ALL messages added this turn so the next
     * user input doesn't see orphaned unanswered assistant/tool messages. */
    while (agent->message_count > pre_turn_count)
        agent_pop_last_message(agent);
    LOG_WARN("Max iterations reached (%d)", agent->max_iterations);
    return strdup("[Dobby: max tool iterations reached. Please try again.]");
}
