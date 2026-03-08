/*
 * history.h — Append-only chronological event log (HISTORY.md)
 *
 * Purpose: auditability, debugging, traceability across sessions.
 *
 * Rules:
 *   - Append-only. Entries are never modified or deleted.
 *   - Entries are grouped by date (## YYYY-MM-DD sections auto-created).
 *   - HISTORY.md is never injected into the LLM context.
 *   - All writes are mutex-protected for concurrent safety.
 *
 * Usage:
 *   history_t *h = history_create("HISTORY.md");
 *   history_log(h, HISTORY_SYSTEM, "Agent Initialized",
 *                     "model: mistral-nemo\nbackend: ollama");
 *   history_destroy(h);
 */
#ifndef HISTORY_H
#define HISTORY_H

#include "../core/dobby.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allowed event categories */
typedef enum {
    HISTORY_SYSTEM       = 0,  /* Agent start/stop, config changes */
    HISTORY_TOOL_EXEC    = 1,  /* Tool invocations and results      */
    HISTORY_MEMORY       = 2,  /* Reads/writes to MEMORY.md         */
    HISTORY_MODEL        = 3,  /* Model or provider changes         */
    HISTORY_API          = 4,  /* Outbound HTTP/API calls           */
    HISTORY_ERROR        = 5,  /* Errors and warnings               */
    HISTORY_FEATURE      = 6,  /* New feature activations           */
    HISTORY_FIX          = 7,  /* Bug fixes / recovery actions      */
} history_category_t;

typedef struct history history_t;

/*
 * Create a history logger writing to the given file path.
 * The file is created with a header if it does not already exist.
 * Returns NULL only on allocation failure (log file errors are soft).
 */
history_t *history_create(const char *path);

/*
 * Append one entry to HISTORY.md.
 *
 * @category : event category (see enum above)
 * @title    : short one-line title, e.g. "Shell Tool Executed"
 * @details  : multi-line body; newlines OK; may be NULL
 *
 * A date section "## YYYY-MM-DD" is inserted automatically when the
 * date changes. Entry format:
 *   ### [Category] Title
 *   - detail line 1
 *   - detail line 2
 */
void history_log(history_t *h,
                       history_category_t category,
                       const char *title,
                       const char *details);

/* Destroy the history logger (flushes and closes file). */
void history_destroy(history_t *h);

#ifdef __cplusplus
}
#endif

#endif /* HISTORY_H */
