/*
 * md_memory.h — Persistent memory backed by a single MEMORY.md file
 *
 * All facts are stored in <memory_dir>/MEMORY.md under canonical sections:
 *   ## User Information
 *   ## Preferences
 *   ## Project Context
 *   ## Important Notes
 *
 * Legacy per-category .md files in the directory are migrated on startup
 * and then deleted.
 */
#ifndef MD_MEMORY_H
#define MD_MEMORY_H

#include "../core/dobby.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct memory memory_t;

/* Create memory subsystem. Migrates legacy files on first run. */
memory_t *memory_create(const char *base_dir);

/*
 * Store a fact. section maps to: "User Information", "Preferences",
 * "Project Context", or "Important Notes" (fuzzy match). If the key
 * already exists in that section it is updated in place.
 */
result_t memory_store(memory_t *mem, const char *section,
                                   const char *key, const char *value);

/* Recall a fact by section and key. Returns NULL if not found (caller frees). */
char *memory_recall(memory_t *mem, const char *section,
                           const char *key);

/* Full-text search of MEMORY.md (caller frees). */
char *memory_search(memory_t *mem, const char *query);

/* Get full MEMORY.md content wrapped for system-prompt injection (caller frees). */
char *memory_get_context(memory_t *mem);

/* Register memory_store / memory_recall / memory_search tools. */
void memory_register_tools(memory_t *mem);

/* Destroy memory subsystem. */
void memory_destroy(memory_t *mem);

#ifdef __cplusplus
}
#endif

#endif /* MD_MEMORY_H */
