/*
 * openai.c — OpenAI-compatible remote LLM provider
 *
 * Supports any backend speaking the OpenAI Chat Completions API.
 * Provider-specific details (URLs, headers, auto-detection) live in
 * src/providers/registry.c — no if-chains needed here.
 */
#include "openai.h"
#include "../http_client.h"
#include "../registry.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private state */

typedef struct {
    char *base_url;   /* resolved URL, e.g. "https://api.groq.com/openai/v1" */
    char *model;
    char *api_key;
    const provider_spec_t *spec; /* points into static registry table   */
    int   total_prompt_tokens;
    int   total_completion_tokens;
} openai_data_t;

/* Request */

static llm_response_t openai_chat_complete(provider_t *self,
                                                  const char       *messages_json,
                                                  const char       *tools_json) {
    llm_response_t resp = {0};
    openai_data_t *data = (openai_data_t *)self->priv;

    /* Build request body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", data->model);
    cJSON_AddBoolToObject(body, "stream", 0);

    cJSON *messages = cJSON_Parse(messages_json);
    if (messages) cJSON_AddItemToObject(body, "messages", messages);

    if (tools_json) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (tools && cJSON_GetArraySize(tools) > 0) {
            cJSON_AddItemToObject(body, "tools", tools);
            /* Send tool_choice:"auto" only if the provider supports it.
             * Ollama does not — it rejects requests with this field.
             * Controlled by fmt.send_tool_choice_auto set at construction. */
            if (self->fmt.send_tool_choice_auto)
                cJSON_AddStringToObject(body, "tool_choice", "auto");
        } else if (tools) {
            cJSON_Delete(tools);
        }
    }

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    /* HTTP POST — use registry extra_headers if present */
    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", data->base_url);

    /* Build "Bearer <key>" auth value */
    char auth_buf[512] = {0};
    if (data->api_key && *data->api_key)
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", data->api_key);

    const char * const *extra = (data->spec && data->spec->extra_headers[0])
                         ? data->spec->extra_headers : NULL;

    http_response_t http_resp = http_post_json_auth_ex(
        url, body_str, *auth_buf ? auth_buf : NULL, extra, 120);

    free(body_str);

    /* Error handling */
    if (http_resp.error) {
        LOG_ERROR("HTTP transport error: %s", http_resp.error);
        resp.error = strdup(http_resp.error);
        http_response_free(&http_resp);
        return resp;
    }

    LOG_DEBUG("API response (HTTP %d): %.2000s",
              http_resp.status_code,
              http_resp.body ? http_resp.body : "(empty)");

    if (http_resp.status_code < 200 || http_resp.status_code >= 300) {
        LOG_ERROR("API HTTP %d — body: %.1000s",
                  http_resp.status_code,
                  http_resp.body ? http_resp.body : "(empty)");

        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "HTTP %d from %s",
                 http_resp.status_code, data->base_url);
        resp.error = strdup(errbuf);

        if (http_resp.body) {
            cJSON *err_json = cJSON_Parse(http_resp.body);
            if (err_json) {
                const char *best = NULL;
                cJSON *err_obj = cJSON_GetObjectItem(err_json, "error");
                if (err_obj) {
                    /* OpenRouter: real message in error.metadata.raw */
                    cJSON *meta = cJSON_GetObjectItem(err_obj, "metadata");
                    if (meta) {
                        cJSON *raw = cJSON_GetObjectItem(meta, "raw");
                        if (raw && cJSON_IsString(raw) && raw->valuestring && *raw->valuestring)
                            best = raw->valuestring;
                    }
                    if (!best) {
                        cJSON *msg = cJSON_GetObjectItem(err_obj, "message");
                        if (msg && cJSON_IsString(msg) && msg->valuestring && *msg->valuestring)
                            best = msg->valuestring;
                    }

                    /* tool_use_failed recovery
                     * Some weaker models (via OpenRouter) cannot emit proper
                     * function-call responses and instead write a JSON array
                     * in their text output.  The provider catches this and
                     * returns HTTP 400 with:
                     *   error.code       = "tool_use_failed"
                     *   error.failed_generation = "<text>\n[{\"name\":...}]"
                     *
                     * We parse the failed_generation, extract the inline tool
                     * call array, normalize it to our standard format, and
                     * return it as a real tool_calls response so the agent
                     * loop can execute it normally.
                     *
                     * Key differences from the standard format the model
                     * should have produced:
                     *   - Uses "parameters" instead of "arguments"
                     *   - May omit "type":"function" wrapper
                     *   - May include prose text before the JSON array
                     */
                    cJSON *code_item = cJSON_GetObjectItem(err_obj, "code");
                    bool is_tool_failed = code_item && cJSON_IsString(code_item) &&
                                         strcmp(code_item->valuestring, "tool_use_failed") == 0;

                    if (is_tool_failed) {
                        cJSON *fg = cJSON_GetObjectItem(err_obj, "failed_generation");
                        if (fg && cJSON_IsString(fg) && fg->valuestring) {
                            /* Find the start of a JSON array in the text */
                            const char *arr_start = strchr(fg->valuestring, '[');
                            while (arr_start) {
                                cJSON *parsed = cJSON_Parse(arr_start);
                                if (parsed && cJSON_IsArray(parsed) &&
                                    cJSON_GetArraySize(parsed) > 0) {

                                    /* Try to normalize each entry */
                                    cJSON *normalized = cJSON_CreateArray();
                                    static unsigned int fg_counter = 0;
                                    cJSON *entry = NULL;
                                    bool   ok    = true;

                                    cJSON_ArrayForEach(entry, parsed) {
                                        cJSON *name = cJSON_GetObjectItem(entry, "name");
                                        if (!name || !cJSON_IsString(name)) { ok = false; break; }

                                        /* Accept both "arguments" (correct) and
                                         * "parameters" (what weak models produce) */
                                        cJSON *args = cJSON_GetObjectItem(entry, "arguments");
                                        if (!args) args = cJSON_GetObjectItem(entry, "parameters");

                                        cJSON *norm_tc   = cJSON_CreateObject();
                                        cJSON *norm_func = cJSON_CreateObject();
                                        char   call_id[32];
                                        snprintf(call_id, sizeof(call_id),
                                                 "fg_%u", ++fg_counter);

                                        cJSON_AddStringToObject(norm_tc, "id",   call_id);
                                        cJSON_AddStringToObject(norm_tc, "type", "function");
                                        cJSON_AddStringToObject(norm_func, "name",
                                                                name->valuestring);

                                        if (args && !cJSON_IsNull(args)) {
                                            char *args_str = cJSON_IsString(args)
                                                ? strdup(args->valuestring)
                                                : cJSON_PrintUnformatted(args);
                                            cJSON_AddStringToObject(norm_func, "arguments",
                                                                    args_str ? args_str : "{}");
                                            free(args_str);
                                        } else {
                                            cJSON_AddStringToObject(norm_func, "arguments", "{}");
                                        }

                                        cJSON_AddItemToObject(norm_tc, "function", norm_func);
                                        cJSON_AddItemToArray(normalized, norm_tc);
                                    }

                                    if (ok && cJSON_GetArraySize(normalized) > 0) {
                                        LOG_WARN("Recovered %d tool call(s) from "
                                                 "failed_generation (weak model fallback)",
                                                 cJSON_GetArraySize(normalized));
                                        free(resp.error);
                                        resp.error     = NULL;
                                        resp.tool_calls = normalized;
                                        cJSON_Delete(parsed);
                                        cJSON_Delete(err_json);
                                        http_response_free(&http_resp);
                                        return resp;
                                    }

                                    cJSON_Delete(normalized);
                                    cJSON_Delete(parsed);
                                }
                                if (parsed) cJSON_Delete(parsed);
                                /* Try next '[' in case first was not the tool array */
                                arr_start = strchr(arr_start + 1, '[');
                            }
                            /* Recovery failed — fall through to normal error path */
                            LOG_WARN("tool_use_failed: could not recover tool calls "
                                     "from failed_generation");
                        }
                    }
                }
                if (!best) {
                    cJSON *msg = cJSON_GetObjectItem(err_json, "message");
                    if (msg && cJSON_IsString(msg) && msg->valuestring && *msg->valuestring)
                        best = msg->valuestring;
                }
                if (best) { free(resp.error); resp.error = strdup(best); }
                cJSON_Delete(err_json);
            }
        }
        http_response_free(&http_resp);
        return resp;
    }

    /* Parse success response */
    cJSON *json = cJSON_Parse(http_resp.body);
    if (!json) {
        LOG_ERROR("Failed to parse JSON response. Body: %.500s",
                  http_resp.body ? http_resp.body : "(empty)");
        resp.error = strdup("Failed to parse JSON response");
        http_response_free(&http_resp);
        return resp;
    }
    http_response_free(&http_resp);

    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    cJSON *choice  = (choices && cJSON_IsArray(choices))
                   ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice ? cJSON_GetObjectItem(choice, "message") : NULL;

    if (!message) {
        char *raw = cJSON_PrintUnformatted(json);
        LOG_ERROR("No choices in response: %.500s", raw ? raw : "(null)");
        free(raw);
        resp.error = strdup("No choices in response");
        cJSON_Delete(json);
        return resp;
    }

    /* finish_reason check — warn if generation was truncated */
    cJSON *finish_reason = choice ? cJSON_GetObjectItem(choice, "finish_reason") : NULL;
    if (finish_reason && cJSON_IsString(finish_reason)) {
        const char *fr = finish_reason->valuestring;
        if (fr && strcmp(fr, "stop") != 0 && strcmp(fr, "tool_calls") != 0) {
            LOG_WARN("OpenAI: finish_reason='%s' — generation may be incomplete", fr);
        }
    }

    /* Text content */
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content) && content->valuestring && content->valuestring[0])
        resp.content = strdup(content->valuestring);

    /* Tool calls */
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
        cJSON *normalized = cJSON_CreateArray();
        cJSON *tc = NULL;
        cJSON_ArrayForEach(tc, tool_calls) {
            cJSON *func = cJSON_GetObjectItem(tc, "function");
            if (!func) continue;

            cJSON *norm_tc   = cJSON_CreateObject();
            cJSON *norm_func = cJSON_CreateObject();

            const char *tc_id = cJSON_GetStringValue(cJSON_GetObjectItem(tc, "id"));
            if (tc_id) cJSON_AddStringToObject(norm_tc, "id", tc_id);
            cJSON_AddStringToObject(norm_tc, "type", "function");

            const char *fname = cJSON_GetStringValue(cJSON_GetObjectItem(func, "name"));
            if (fname) cJSON_AddStringToObject(norm_func, "name", fname);

            /* arguments must be a JSON-encoded string (OpenAI/Groq spec) */
            cJSON *tc_args = cJSON_GetObjectItem(func, "arguments");
            if (tc_args) {
                if (cJSON_IsString(tc_args)) {
                    cJSON_AddStringToObject(norm_func, "arguments", tc_args->valuestring);
                } else {
                    char *args_str = cJSON_PrintUnformatted(tc_args);
                    cJSON_AddStringToObject(norm_func, "arguments", args_str ? args_str : "{}");
                    free(args_str);
                }
            } else {
                cJSON_AddStringToObject(norm_func, "arguments", "{}");
            }

            cJSON_AddItemToObject(norm_tc, "function", norm_func);
            cJSON_AddItemToArray(normalized, norm_tc);
        }

        if (cJSON_GetArraySize(normalized) > 0) {
            resp.tool_calls = normalized;
        } else {
            /* All entries failed validation — don't attach empty array */
            cJSON_Delete(normalized);
        }
    }

    /* Token usage */
    cJSON *usage = cJSON_GetObjectItem(json, "usage");
    if (usage) {
        cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
        if (pt) { resp.prompt_tokens     = pt->valueint; data->total_prompt_tokens     += pt->valueint; }
        if (ct) { resp.completion_tokens = ct->valueint; data->total_completion_tokens += ct->valueint; }
    }

    cJSON_Delete(json);
    return resp;
}

static void openai_destroy(provider_t *self) {
    openai_data_t *data = (openai_data_t *)self->priv;
    if (data) {
        LOG_DEBUG("OpenAI provider done — prompt: %d tokens, completion: %d tokens",
                  data->total_prompt_tokens, data->total_completion_tokens);
        free(data->base_url);
        free(data->model);
        free(data->api_key);
        free(data);
    }
    free(self);
}

/* Public constructor */

provider_t *openai_create(const char *model,
                                       const char *api_url,
                                       const char *api_key,
                                       const char *provider_name) {
    /* Look up registry spec by explicit name, then auto-detect from key/url */
    const provider_spec_t *spec = registry_find_by_name(provider_name);
    if (!spec) spec = registry_detect(api_key, api_url);

    /* Resolve base URL: explicit > registry default */
    const char *resolved_url = registry_resolve_url(spec, api_url);
    if (!resolved_url) {
        LOG_ERROR("No URL for provider '%s' — set url in dobby.conf or API_URL",
                  provider_name ? provider_name : "(unknown)");
        return NULL;
    }

    openai_data_t *data = calloc(1, sizeof(openai_data_t));
    if (!data) return NULL;

    data->model    = strdup(model ? model : "gpt-4o");
    data->base_url = strdup(resolved_url);
    data->spec     = spec;

    /* API key: explicit arg → env var → empty (local/no-auth servers) */
    if (api_key && *api_key) {
        data->api_key = strdup(api_key);
    } else {
        const char *env_key = getenv("API_KEY");
        data->api_key = strdup(env_key ? env_key : "");
    }

    provider_t *provider = calloc(1, sizeof(provider_t));
    if (!provider) { free(data->model); free(data->base_url); free(data->api_key); free(data); return NULL; }

    provider->name          = provider_name ? provider_name : "openai";
    provider->model         = data->model;
    provider->chat_complete = openai_chat_complete;
    provider->destroy       = openai_destroy;
    provider->priv          = data;

    /* OpenAI-compatible wire format:
     *   - tool result messages: "name" field not required (OpenAI spec)
     *   - arguments always returned as JSON-encoded string
     *   - tool_choice:"auto" improves reliability on Groq/OpenAI/Mistral
     *   - omit content on assistant tool-call-only messages (safest) */
    provider->fmt.tool_result_include_name          = false;
    provider->fmt.tool_args_as_object               = false;
    provider->fmt.send_tool_choice_auto             = true;
    provider->fmt.assistant_tool_call_empty_content = false;

    LOG_DEBUG("OpenAI-compat provider: name=%s model=%s url=%s key=%s",
              provider->name, data->model, data->base_url,
              data->api_key && *data->api_key ? "(set)" : "(none)");

    return provider;
}
