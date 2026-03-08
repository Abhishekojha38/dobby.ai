/*
 * registry.c — LLM provider registry
 *
 * Adding a new provider: add one entry to PROVIDERS below.
 * Order matters for auto-detection priority (first match wins).
 */
#include "registry.h"
#include <string.h>
#include <strings.h>

/* Provider table */

static const provider_spec_t PROVIDERS[] = {
    {
        .name               = "openrouter",
        .default_url        = "https://openrouter.ai/api/v1",
        .detect_key_prefix  = "sk-or-",
        .detect_url_keyword = "openrouter",
        .extra_headers      = {
            "HTTP-Referer: https://github.com/dobby-ai/dobby",
            "X-Title: Dobby AI",
            NULL, NULL
        },
    },
    {
        .name               = "groq",
        .default_url        = "https://api.groq.com/openai/v1",
        .detect_key_prefix  = "gsk_",
        .detect_url_keyword = "groq",
        .extra_headers      = { NULL, NULL, NULL, NULL },
    },
    {
        .name               = "together",
        .default_url        = "https://api.together.xyz/v1",
        .detect_key_prefix  = NULL,
        .detect_url_keyword = "together",
        .extra_headers      = { NULL, NULL, NULL, NULL },
    },
    {
        .name               = "openai",
        .default_url        = "https://api.openai.com/v1",
        .detect_key_prefix  = "sk-",
        .detect_url_keyword = "openai",
        .extra_headers      = { NULL, NULL, NULL, NULL },
    },
    /* Sentinel */
    { NULL, NULL, NULL, NULL, { NULL, NULL, NULL, NULL } }
};

/* Lookup helpers */

const provider_spec_t *registry_find_by_name(const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 0; PROVIDERS[i].name; i++) {
        if (!strcasecmp(PROVIDERS[i].name, name))
            return &PROVIDERS[i];
    }
    return NULL;
}

const provider_spec_t *registry_detect(const char *api_key,
                                                    const char *api_url) {
    for (int i = 0; PROVIDERS[i].name; i++) {
        const provider_spec_t *s = &PROVIDERS[i];
        if (s->detect_key_prefix && api_key && *api_key) {
            size_t plen = strlen(s->detect_key_prefix);
            if (strncmp(api_key, s->detect_key_prefix, plen) == 0)
                return s;
        }
        if (s->detect_url_keyword && api_url && *api_url) {
            if (strstr(api_url, s->detect_url_keyword))
                return s;
        }
    }
    return NULL;
}

const char *registry_resolve_url(const provider_spec_t *spec,
                                       const char *api_url) {
    if (api_url && *api_url) return api_url;
    if (spec && spec->default_url) return spec->default_url;
    return NULL;
}
