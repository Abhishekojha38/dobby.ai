/*
 * ollama.c — Ollama LLM provider implementation
 *
 * Uses Ollama's /api/chat endpoint with tool calling support.
 */
#include "ollama.h"
#include "../http_client.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *base_url;
    char *model;
    int   total_prompt_tokens;
    int   total_completion_tokens;
} ollama_data_t;

/* Provider interface implementations */



static llm_response_t ollama_chat_complete(provider_t *self,
                                                  const char *messages_json,
                                                  const char *tools_json) {
    llm_response_t resp = {0};
    ollama_data_t *data = (ollama_data_t *)self->priv;

    /* Build request body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", data->model);
    cJSON_AddBoolToObject(body, "stream", 0);

    /* Parse and add messages */
    cJSON *messages = cJSON_Parse(messages_json);
    if (messages) {
        cJSON_AddItemToObject(body, "messages", messages);
    }

    /* Parse and add tools if present */
    if (tools_json) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (tools && cJSON_GetArraySize(tools) > 0) {
            cJSON_AddItemToObject(body, "tools", tools);
        } else if (tools) {
            cJSON_Delete(tools);
        }
    }

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    /* Make HTTP request */
    char url[512];
    snprintf(url, sizeof(url), "%s/api/chat", data->base_url);

    http_response_t http_resp = http_post_json(url, body_str, 120);
    free(body_str);

    if (http_resp.error) {
        resp.error = strdup(http_resp.error);
        http_response_free(&http_resp);
        return resp;
    }

    /* Parse response */
    cJSON *json = cJSON_Parse(http_resp.body);

    if (!json) {
        LOG_ERROR("Failed to parse Ollama response. Body (first 500 chars): %.500s",
                  http_resp.body ? http_resp.body : "(null)");
        resp.error = strdup("Failed to parse Ollama response (invalid JSON)");
        http_response_free(&http_resp);
        return resp;
    }

    http_response_free(&http_resp);

    /* Check for error in response */
    cJSON *err = cJSON_GetObjectItem(json, "error");
    if (err && cJSON_IsString(err)) {
        resp.error = strdup(err->valuestring);
        cJSON_Delete(json);
        return resp;
    }

    /* Check for truncated generation.
     * Ollama sets done_reason="length" when the model hit the context limit
     * mid-generation — tool call arguments may be incomplete JSON in this case.
     * Surface it as an error so the agent can report it instead of silently
     * executing a tool with empty / garbled arguments. */
    cJSON *done_reason = cJSON_GetObjectItem(json, "done_reason");
    if (done_reason && cJSON_IsString(done_reason) &&
        strcmp(done_reason->valuestring, "stop") != 0 &&
        strcmp(done_reason->valuestring, "tool_calls") != 0) {
        /* Non-stop finish: length limit, error, etc. */
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf),
                 "Generation stopped early (done_reason: %s) — "
                 "try a shorter prompt or a model with a larger context window",
                 done_reason->valuestring);
        LOG_WARN("Ollama: %s", errbuf);
        /* Fall through — we may still have usable content.
         * Only hard-error if there are no tool_calls either. */
        cJSON *msg_check = cJSON_GetObjectItem(json, "message");
        cJSON *tc_check  = msg_check
                         ? cJSON_GetObjectItem(msg_check, "tool_calls") : NULL;
        bool has_truncated_tools = tc_check && cJSON_GetArraySize(tc_check) > 0;
        if (has_truncated_tools) {
            resp.error = strdup(errbuf);
            cJSON_Delete(json);
            return resp;
        }
    }

    /* Extract message */
    cJSON *message = cJSON_GetObjectItem(json, "message");
    if (message) {
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content)) {
            resp.content = strdup(content->valuestring);
        }

        /* Extract tool calls.
         *
         * Ollama returns arguments as a JSON *object*, not a JSON-encoded string.
         * We normalize to a JSON-encoded string here so the rest of the pipeline
         * (agent.c build_messages_json, parse_tool_arguments) is consistent.
         *
         * Key correctness rule: the stored `arguments` value must always be a
         * valid JSON string that can be round-tripped through cJSON_Parse().
         * If serialization fails for any item, skip that tool call and log it
         * rather than propagating a broken entry that will crash later. */
        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        if (tool_calls && cJSON_IsArray(tool_calls) &&
            cJSON_GetArraySize(tool_calls) > 0) {

            cJSON *normalized = cJSON_CreateArray();
            int size = cJSON_GetArraySize(tool_calls);

            for (int i = 0; i < size; i++) {
                cJSON *tc   = cJSON_GetArrayItem(tool_calls, i);
                cJSON *func = cJSON_GetObjectItem(tc, "function");
                if (!func) {
                    LOG_WARN("Ollama tool_call[%d]: missing 'function' key — skipped", i);
                    continue;
                }

                cJSON *name = cJSON_GetObjectItem(func, "name");
                cJSON *args = cJSON_GetObjectItem(func, "arguments");

                if (!name || !cJSON_IsString(name) || !name->valuestring[0]) {
                    LOG_WARN("Ollama tool_call[%d]: missing or empty function name — skipped", i);
                    continue;
                }

                /* Serialize arguments to a JSON string.
                 * Ollama returns an object; some future versions may return a
                 * pre-encoded string — handle both. */
                char *args_str = NULL;
                if (!args) {
                    args_str = strdup("{}");
                } else if (cJSON_IsString(args)) {
                    /* Already a string — validate it parses cleanly */
                    cJSON *probe = cJSON_Parse(args->valuestring);
                    if (!probe) {
                        LOG_ERROR("Ollama tool_call[%d] '%s': arguments is a string "
                                  "but not valid JSON: %.200s — skipped",
                                  i, name->valuestring, args->valuestring);
                        continue;
                    }
                    cJSON_Delete(probe);
                    args_str = strdup(args->valuestring);
                } else {
                    /* Object (normal Ollama path) — serialize */
                    args_str = cJSON_PrintUnformatted(args);
                    if (!args_str) {
                        LOG_ERROR("Ollama tool_call[%d] '%s': cJSON_PrintUnformatted "
                                  "returned NULL — skipped", i, name->valuestring);
                        continue;
                    }
                    /* Validate the round-trip */
                    cJSON *probe = cJSON_Parse(args_str);
                    if (!probe) {
                        LOG_ERROR("Ollama tool_call[%d] '%s': serialized args not "
                                  "valid JSON: %.200s — skipped",
                                  i, name->valuestring, args_str);
                        free(args_str);
                        continue;
                    }
                    cJSON_Delete(probe);
                }

                /* Build normalized tool call entry */
                cJSON *norm_tc   = cJSON_CreateObject();
                cJSON *norm_func = cJSON_CreateObject();

                /* Unique monotonic ID — reusing call_0/call_1 per response
                 * causes the model to misattribute tool results. */
                static unsigned int call_counter = 0;
                char call_id[32];
                snprintf(call_id, sizeof(call_id), "call_%u", ++call_counter);
                cJSON_AddStringToObject(norm_tc, "id",   call_id);
                cJSON_AddStringToObject(norm_tc, "type", "function");
                cJSON_AddStringToObject(norm_func, "name",      name->valuestring);
                cJSON_AddStringToObject(norm_func, "arguments", args_str);
                free(args_str);

                cJSON_AddItemToObject(norm_tc, "function", norm_func);
                cJSON_AddItemToArray(normalized, norm_tc);
            }

            /* Only attach tool_calls if at least one survived validation */
            if (cJSON_GetArraySize(normalized) > 0) {
                resp.tool_calls = normalized;
            } else {
                cJSON_Delete(normalized);
                if (!resp.content) {
                    resp.error = strdup(
                        "Model requested tool calls but all had invalid arguments. "
                        "Try rephrasing your request.");
                }
            }
        }
    }

    /* Extract token usage */
    cJSON *prompt_eval = cJSON_GetObjectItem(json, "prompt_eval_count");
    cJSON *eval_count  = cJSON_GetObjectItem(json, "eval_count");
    if (prompt_eval) {
        resp.prompt_tokens = prompt_eval->valueint;
        data->total_prompt_tokens += resp.prompt_tokens;
    }
    if (eval_count) {
        resp.completion_tokens = eval_count->valueint;
        data->total_completion_tokens += resp.completion_tokens;
    }

    cJSON_Delete(json);
    return resp;
}

static void ollama_destroy(provider_t *self) {
    ollama_data_t *data = (ollama_data_t *)self->priv;
    if (data) {
        LOG_DEBUG("Ollama usage — prompt: %d tokens, completion: %d tokens",
                 data->total_prompt_tokens, data->total_completion_tokens);
        free(data->base_url);
        free(data->model);
        free(data);
    }
    free(self);
}

/* Public constructor */

provider_t *ollama_create(const char *model, const char *base_url) {
    ollama_data_t *data = calloc(1, sizeof(ollama_data_t));
    if (!data) return NULL;
    data->model    = strdup(model    ? model    : "llama3.2");
    data->base_url = strdup(base_url ? base_url : "http://localhost:11434");

    provider_t *provider = calloc(1, sizeof(provider_t));
    if (!provider) { free(data->model); free(data->base_url); free(data); return NULL; }

    provider->name          = "ollama";
    provider->model         = data->model;
    provider->chat_complete = ollama_chat_complete;
    provider->destroy       = ollama_destroy;
    provider->priv          = data;

    /* Ollama wire format quirks:
     *   - tool result messages require "name" field to correlate results
     *   - returns tool call arguments as JSON object (not pre-serialized string)
     *   - does not support tool_choice field
     *   - omitting content on assistant tool-call messages is correct */
    provider->fmt.tool_result_include_name        = true;
    provider->fmt.tool_args_as_object             = true;
    provider->fmt.send_tool_choice_auto           = false;
    provider->fmt.assistant_tool_call_empty_content = false;

    LOG_DEBUG("Ollama provider: model=%s url=%s", data->model, data->base_url);
    return provider;
}
