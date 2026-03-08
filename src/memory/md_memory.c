/* _GNU_SOURCE for strcasestr */

/*
 * md_memory.c — Single-file persistent memory backed by MEMORY.md
 *
 * Architecture:
 *   All persistent facts live in ONE file: <memory_dir>/MEMORY.md
 *   Format is structured markdown with H2 section headings:
 *
 *     # Long-term Memory
 *     ## User Information
 *     - key: value
 *     ## Preferences
 *     - key: value
 *
 *   Allowed sections (canoncial):
 *     - User Information
 *     - Preferences
 *     - Project Context
 *     - Important Notes
 *
 *   The LLM stores facts via memory_store(section, key, value).
 *   The section maps to one of the H2 headers above.
 *   The key+value are appended as a bullet under that section.
 *
 * Thread safety: a pthread mutex guards all file I/O.
 *
 * Migration: if legacy per-category .md files exist in the directory
 * (other than MEMORY.md), their content is appended to MEMORY.md
 * under "Important Notes" and the files are deleted.
 */
#define _GNU_SOURCE
#include "md_memory.h"
#include "../core/log.h"
#include "../core/cJSON.h"
#include "../agent/tool_registry.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEMORY_FILE   "MEMORY.md"
#define MAX_FILE_SIZE (256 * 1024)   /* 256 KB sanity cap */

/* Canonical sections — any store call maps to one of these. */
static const char *CANONICAL_SECTIONS[] = {
    "User Information",
    "Preferences",
    "Project Context",
    "Important Notes",
    NULL
};

struct memory {
    char           *base_dir;
    char            path[512];      /* full path to MEMORY.md */
    pthread_mutex_t mu;
};

/* Internal helpers */

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) mkdir(path, 0755);
}

/* Map an arbitrary category string to the nearest canonical section.
 * Matches case-insensitively on substrings; falls back to "Important Notes". */
static const char *canonicalise_section(const char *category) {
    if (!category) return CANONICAL_SECTIONS[3];
    for (int i = 0; CANONICAL_SECTIONS[i]; i++) {
        if (strcasestr(CANONICAL_SECTIONS[i], category) ||
            strcasestr(category, CANONICAL_SECTIONS[i])) {
            return CANONICAL_SECTIONS[i];
        }
    }
    /* keyword heuristics */
    if (strcasestr(category, "user") || strcasestr(category, "person"))
        return CANONICAL_SECTIONS[0];
    if (strcasestr(category, "pref") || strcasestr(category, "setting"))
        return CANONICAL_SECTIONS[1];
    if (strcasestr(category, "project") || strcasestr(category, "task"))
        return CANONICAL_SECTIONS[2];
    return CANONICAL_SECTIONS[3];
}

/* Read entire MEMORY.md into a heap string. Returns "" if not found. */
static char *read_memory_file(const memory_t *mem) {
    FILE *f = fopen(mem->path, "r");
    if (!f) return strdup("");

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > MAX_FILE_SIZE) { fclose(f); return strdup(""); }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return strdup(""); }
    buf[fread(buf, 1, (size_t)sz, f)] = '\0';
    fclose(f);
    return buf;
}

/* Write string back atomically via a temp file. */
static result_t write_memory_file(const memory_t *mem,
                                         const char *content) {
    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", mem->path);

    FILE *f = fopen(tmp, "w");
    if (!f) return err(ERR_IO, "Cannot write %s: %s",
                              tmp, strerror(errno));
    fputs(content, f);
    fclose(f);

    if (rename(tmp, mem->path) != 0) {
        unlink(tmp);
        return err(ERR_IO, "Cannot rename %s: %s",
                          tmp, strerror(errno));
    }
    return ok();
}

/*
 * Create a fresh MEMORY.md with the canonical structure.
 * Called only when the file does not exist yet.
 */
static void create_initial_memory(const memory_t *mem) {
    FILE *f = fopen(mem->path, "w");
    if (!f) return;
    fprintf(f,
        "# Long-term Memory\n\n"
        "This file stores important information that should persist across sessions.\n\n"
        "## User Information\n\n"
        "## Preferences\n\n"
        "## Project Context\n\n"
        "## Important Notes\n");
    fclose(f);
    LOG_WARN("Created new MEMORY.md at %s", mem->path);
}

/*
 * Migrate legacy .md files (anything that is not MEMORY.md) into MEMORY.md
 * under the "Important Notes" section, then delete them.
 */
static void migrate_legacy_files(memory_t *mem) {
    DIR *dir = opendir(mem->base_dir);
    if (!dir) return;

    struct dirent *entry;
    bool any = false;

    while ((entry = readdir(dir)) != NULL) {
        if (!str_ends_with(entry->d_name, ".md")) continue;
        if (strcmp(entry->d_name, MEMORY_FILE) == 0) continue;

        char legacy_path[512];
        snprintf(legacy_path, sizeof(legacy_path),
                 "%s/%s", mem->base_dir, entry->d_name);

        FILE *f = fopen(legacy_path, "r");
        if (!f) continue;

        strbuf_t sb; strbuf_init(&sb, 1024);
        char line[1024];
        while (fgets(line, sizeof(line), f)) strbuf_append(&sb, line);
        fclose(f);

        if (sb.len > 0) {
            /* Append to MEMORY.md under Important Notes */
            memory_store(mem, "Important Notes",
                               entry->d_name, sb.data);
            any = true;
        }
        strbuf_free(&sb);
        unlink(legacy_path);
        LOG_WARN("Migrated legacy memory file %s into MEMORY.md", entry->d_name);
    }
    closedir(dir);
    (void)any;
}

/* Public API */

memory_t *memory_create(const char *base_dir) {
    memory_t *mem = calloc(1, sizeof(memory_t));
    mem->base_dir = strdup(base_dir ? base_dir : "memory");
    snprintf(mem->path, sizeof(mem->path),
             "%s/%s", mem->base_dir, MEMORY_FILE);
    pthread_mutex_init(&mem->mu, NULL);

    ensure_dir(mem->base_dir);

    struct stat st;
    if (stat(mem->path, &st) != 0) {
        create_initial_memory(mem);
    }

    /* Migrate any legacy per-category .md files */
    migrate_legacy_files(mem);

    LOG_DEBUG("Memory ready: %s", mem->path);
    return mem;
}

/*
 * memory_store - Insert or update a bullet under the given section.
 *
 * The category is mapped to a canonical section heading.
 * If the key already exists, its line is replaced.
 * If the section doesn't exist, it is appended.
 */
result_t memory_store(memory_t *mem, const char *category,
                                   const char *key, const char *value) {
    if (!key || !*key || !value) {
        return err(ERR_GENERIC, "key and value are required");
    }

    const char *section = canonicalise_section(category);
    char bullet[2048];
    snprintf(bullet, sizeof(bullet), "- %s: %s", key, value);

    pthread_mutex_lock(&mem->mu);

    char *content = read_memory_file(mem);
    strbuf_t out; strbuf_init(&out, strlen(content) + 256);

    bool in_target   = false;   /* currently under the target section */
    bool key_written = false;   /* already wrote the updated line */
    bool section_found = false;

    char *line = content;
    char *next;

    while (*line) {
        /* Find end of this line */
        next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line + 1) : strlen(line);
        char linebuf[2048];
        size_t copy = len < sizeof(linebuf) - 1 ? len : sizeof(linebuf) - 1;
        memcpy(linebuf, line, copy);
        linebuf[copy] = '\0';

        /* Detect H2 heading */
        if (strncmp(linebuf, "## ", 3) == 0) {
            char heading[256];
            strncpy(heading, linebuf + 3, sizeof(heading) - 1);
            heading[sizeof(heading) - 1] = '\0';
            /* strip newline */
            char *nl = strchr(heading, '\n'); if (nl) *nl = '\0';
            char *cr = strchr(heading, '\r'); if (cr) *cr = '\0';

            if (in_target && !key_written) {
                /* End of section reached without finding key — append it */
                strbuf_appendf(&out, "%s\n", bullet);
                key_written = true;
            }

            in_target = (strcmp(heading, section) == 0);
            if (in_target) section_found = true;
        }

        /* Check if this line is the key we want to update */
        if (in_target && strncmp(linebuf, "- ", 2) == 0) {
            /* Extract key part before ':' */
            char existing_key[512] = {0};
            const char *colon = strchr(linebuf + 2, ':');
            if (colon) {
                size_t klen = (size_t)(colon - (linebuf + 2));
                if (klen < sizeof(existing_key))
                    memcpy(existing_key, linebuf + 2, klen);
            }
            char *trimmed_key = str_trim(existing_key);
            if (strcmp(trimmed_key, key) == 0) {
                /* Replace this line */
                strbuf_appendf(&out, "%s\n", bullet);
                key_written = true;
                line += len;
                continue;
            }
        }

        strbuf_append_len(&out, linebuf, len);
        line += len;
    }
    free(content);

    /* Key not yet written — handle remaining cases */
    if (!key_written) {
        if (!section_found) {
            /* Section doesn't exist — append it */
            strbuf_appendf(&out, "\n## %s\n\n%s\n", section, bullet);
        } else {
            /* Section existed but key wasn't there — already appended above
             * unless section was last and loop ended while still in_target */
            strbuf_appendf(&out, "%s\n", bullet);
        }
    }

    result_t r = write_memory_file(mem, out.data);
    strbuf_free(&out);
    pthread_mutex_unlock(&mem->mu);

    if (r.status == OK)
        LOG_DEBUG("Memory stored [%s] %s", section, key);
    return r;
}

/*
 * memory_recall - Find and return the value for a key in any section.
 * Returns heap-allocated string (caller frees), or NULL if not found.
 */
char *memory_recall(memory_t *mem, const char *category,
                           const char *key) {
    pthread_mutex_lock(&mem->mu);
    char *content = read_memory_file(mem);
    pthread_mutex_unlock(&mem->mu);

    const char *section = canonicalise_section(category);
    char *result = NULL;
    bool in_target = false;

    char *line = content;
    char *next;

    while (*line) {
        next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line + 1) : strlen(line);
        char linebuf[2048];
        size_t copy = len < sizeof(linebuf) - 1 ? len : sizeof(linebuf) - 1;
        memcpy(linebuf, line, copy); linebuf[copy] = '\0';

        if (strncmp(linebuf, "## ", 3) == 0) {
            char heading[256]; strncpy(heading, linebuf + 3, sizeof(heading) - 1);
            heading[sizeof(heading) - 1] = '\0';
            char *nl = strchr(heading, '\n'); if (nl) *nl = '\0';
            in_target = (strcmp(str_trim(heading), section) == 0);
        } else if (in_target && strncmp(linebuf, "- ", 2) == 0) {
            const char *colon = strchr(linebuf + 2, ':');
            if (colon) {
                char existing_key[512] = {0};
                size_t klen = (size_t)(colon - (linebuf + 2));
                if (klen < sizeof(existing_key))
                    memcpy(existing_key, linebuf + 2, klen);
                if (strcmp(str_trim(existing_key), key) == 0) {
                    { char _val[512]; strncpy(_val, colon + 1, sizeof(_val)-1); _val[sizeof(_val)-1] = '\0'; result = strdup(str_trim(_val)); }
                    break;
                }
            }
        }
        line += len;
    }

    free(content);
    return result;
}

/* Search MEMORY.md for a keyword — returns matching lines (caller frees). */
char *memory_search(memory_t *mem, const char *query) {
    pthread_mutex_lock(&mem->mu);
    char *content = read_memory_file(mem);
    pthread_mutex_unlock(&mem->mu);

    strbuf_t sb; strbuf_init(&sb, 512);
    int matches = 0;
    char *line = content, *next;

    while (*line) {
        next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line + 1) : strlen(line);
        char linebuf[2048];
        size_t copy = len < sizeof(linebuf) - 1 ? len : sizeof(linebuf) - 1;
        memcpy(linebuf, line, copy); linebuf[copy] = '\0';

        if (strcasestr(linebuf, query)) {
            if (matches == 0)
                strbuf_appendf(&sb, "Results for \"%s\":\n\n", query);
            strbuf_append(&sb, "  ");
            strbuf_append_len(&sb, linebuf, len);
            matches++;
        }
        line += len;
    }
    free(content);

    if (matches == 0)
        strbuf_appendf(&sb, "No memories matching \"%s\".", query);

    char *r = strdup(sb.data);
    strbuf_free(&sb);
    return r;
}

/* Return entire MEMORY.md content for context injection (caller frees). */
char *memory_get_context(memory_t *mem) {
    pthread_mutex_lock(&mem->mu);
    char *content = read_memory_file(mem);
    pthread_mutex_unlock(&mem->mu);

    strbuf_t sb; strbuf_init(&sb, strlen(content) + 256);
    strbuf_append(&sb,
        "## Persistent Memory\n"
        "Below is your long-term memory. Use memory_store to update it, "
        "memory_recall to look up a specific fact.\n\n");
    strbuf_append(&sb, content);
    free(content);

    char *r = strdup(sb.data);
    strbuf_free(&sb);
    return r;
}

void memory_destroy(memory_t *mem) {
    if (!mem) return;
    pthread_mutex_destroy(&mem->mu);
    free(mem->base_dir);
    free(mem);
}

/* Tool callbacks */

static char *tool_memory_store(const cJSON *args, void *ud) {
    memory_t *mem  = (memory_t *)ud;
    const char *category = cJSON_GetStringValue(cJSON_GetObjectItem(args, "section"));
    const char *key      = cJSON_GetStringValue(cJSON_GetObjectItem(args, "key"));
    const char *value    = cJSON_GetStringValue(cJSON_GetObjectItem(args, "value"));

    /* Accept "category" as an alias for "section" */
    if (!category) category = cJSON_GetStringValue(cJSON_GetObjectItem(args, "category"));

    result_t r = memory_store(mem, category, key, value);
    if (r.status != OK) {
        char *msg = strdup(r.message);
        result_free(&r);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", msg);
        free(msg);
        char *j = cJSON_PrintUnformatted(e); cJSON_Delete(e);
        return j;
    }
    return strdup("{\"status\":\"stored\"}");
}

static char *tool_memory_recall(const cJSON *args, void *ud) {
    memory_t *mem  = (memory_t *)ud;
    const char *category = cJSON_GetStringValue(cJSON_GetObjectItem(args, "section"));
    if (!category) category = cJSON_GetStringValue(cJSON_GetObjectItem(args, "category"));
    const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(args, "key"));

    char *val = memory_recall(mem, category, key);
    cJSON *obj = cJSON_CreateObject();
    if (val) { cJSON_AddStringToObject(obj, "value", val); free(val); }
    else      { cJSON_AddStringToObject(obj, "error", "not found"); }
    char *j = cJSON_PrintUnformatted(obj); cJSON_Delete(obj);
    return j;
}

static char *tool_memory_search(const cJSON *args, void *ud) {
    memory_t *mem = (memory_t *)ud;
    const char *query   = cJSON_GetStringValue(cJSON_GetObjectItem(args, "query"));
    if (!query || !*query) return strdup("{\"error\":\"query required\"}");
    return memory_search(mem, query);
}

void memory_register_tools(memory_t *mem) {
    static const tool_t tools[] = {
        {
            .name        = "memory_store",
            .description = "Save an important fact or preference to MEMORY.md. "
                           "section must be one of: 'User Information', 'Preferences', "
                           "'Project Context', 'Important Notes'. "
                           "key is a short label; value is the detailed content.",
            .parameters_schema =
                "{\"type\":\"object\",\"properties\":{"
                "\"section\":{\"type\":\"string\","
                  "\"description\":\"One of: User Information, Preferences, Project Context, Important Notes\"},"
                "\"key\":{\"type\":\"string\",\"description\":\"Short label\"},"
                "\"value\":{\"type\":\"string\",\"description\":\"Detailed fact to remember\"}"
                "},\"required\":[\"section\",\"key\",\"value\"]}",
            .execute   = tool_memory_store,
            .user_data = NULL, /* patched below */
        },
        {
            .name        = "memory_recall",
            .description = "Recall a specific fact from MEMORY.md by section and key.",
            .parameters_schema =
                "{\"type\":\"object\",\"properties\":{"
                "\"section\":{\"type\":\"string\"},"
                "\"key\":{\"type\":\"string\"}"
                "},\"required\":[\"section\",\"key\"]}",
            .execute   = tool_memory_recall,
            .user_data = NULL,
        },
        {
            .name        = "memory_search",
            .description = "Full-text search of MEMORY.md.",
            .parameters_schema =
                "{\"type\":\"object\",\"properties\":{"
                "\"query\":{\"type\":\"string\"}"
                "},\"required\":[\"query\"]}",
            .execute   = tool_memory_search,
            .user_data = NULL,
        },
    };

    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        tool_t t = tools[i];
        t.user_data    = mem;
        tool_register(&t);
    }
}
