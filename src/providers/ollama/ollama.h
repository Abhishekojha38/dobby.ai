/*
 * ollama.h — Ollama LLM provider
 */
#ifndef OLLAMA_H
#define OLLAMA_H

#include "../provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create an Ollama provider instance.
 * model:    model name (e.g. "llama3.2", "qwen2.5")
 * base_url: Ollama API base (default "http://localhost:11434") */
provider_t *ollama_create(const char *model, const char *base_url);

#ifdef __cplusplus
}
#endif

#endif /* OLLAMA_H */
