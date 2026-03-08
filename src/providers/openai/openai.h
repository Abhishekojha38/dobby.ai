/*
 * openai.h — OpenAI-compatible remote LLM provider
 *
 * Works with any API speaking the OpenAI Chat Completions format:
 *   OpenAI, Groq, Together AI, OpenRouter, vLLM, llama.cpp, etc.
 *
 * Provider-specific URLs and headers come from the registry (registry.c).
 * No per-provider if-chains in this file.
 */
#ifndef OPENAI_H
#define OPENAI_H

#include "../provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * openai_create — Create an OpenAI-compatible provider.
 *
 * model:         Model name (e.g. "gpt-4o", "meta-llama/llama-3.3-70b-instruct")
 * api_url:       Base URL override. NULL = use registry default for provider_name.
 * api_key:       Bearer token. NULL = read API_KEY env var.
 * provider_name: Registry lookup key ("openrouter", "groq", "openai", etc.).
 *                NULL = auto-detect from api_key prefix or api_url keyword.
 *
 * Returns NULL on failure. Connection is tested lazily on first call.
 */
provider_t *openai_create(const char *model,
                                       const char *api_url,
                                       const char *api_key,
                                       const char *provider_name);

#ifdef __cplusplus
}
#endif

#endif /* OPENAI_H */
