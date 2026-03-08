/*
 * registry.h — LLM provider registry
 *
 * Single source of truth for provider metadata.
 * Adding a new provider: add one entry to PROVIDERS in registry.c.
 * No other files need to change.
 */
#ifndef PROVIDER_REGISTRY_H
#define PROVIDER_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum extra headers per provider (NULL-terminated). */
#define REGISTRY_MAX_HEADERS 4

typedef struct {
    const char *name;               /* config type name: "openrouter", "groq" … */
    const char *default_url;        /* base URL — NULL means user must supply    */
    const char *detect_key_prefix;  /* auto-detect by api_key prefix e.g "sk-or-" */
    const char *detect_url_keyword; /* auto-detect by substring in api_url      */
    const char *extra_headers[REGISTRY_MAX_HEADERS]; /* NULL-terminated   */
} provider_spec_t;

/*
 * registry_find_by_name — look up spec by config type name.
 * Returns NULL if not found.
 */
const provider_spec_t *registry_find_by_name(const char *name);

/*
 * registry_detect — auto-detect provider from api_key or api_url.
 * Checks key prefix first, then URL keyword.
 * Returns NULL if no match.
 */
const provider_spec_t *registry_detect(const char *api_key,
                                                    const char *api_url);

/*
 * registry_resolve_url — return the URL to use.
 * If api_url is non-empty, use it.
 * Otherwise fall back to spec->default_url.
 * Returns NULL if neither is set.
 */
const char *registry_resolve_url(const provider_spec_t *spec,
                                       const char *api_url);

#ifdef __cplusplus
}
#endif

#endif /* PROVIDER_REGISTRY_H */
