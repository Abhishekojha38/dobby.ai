/*
 * history.c — Append-only HISTORY.md writer
 */
#define _GNU_SOURCE
#include "history.h"
#include "../core/log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *CATEGORY_NAMES[] = {
    "System Event",
    "Tool Execution",
    "Memory Update",
    "Model Change",
    "API Call",
    "Error",
    "Feature",
    "Fix",
};

struct history {
    char           *path;
    char            last_date[16];  /* "YYYY-MM-DD" of the last written entry */
    pthread_mutex_t mu;
};

/* Helpers */

static void get_date(char date[16]) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(date, 16, "%Y-%m-%d", &tm_buf);
}

static void get_timestamp(char ts[32]) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(ts, 32, "%H:%M:%S", &tm_buf);
}

/*
 * Determine the date of the last entry in the file so we know whether
 * to insert a new ## YYYY-MM-DD section. Reads the last 4 KB only.
 */
static void detect_last_date(history_t *h) {
    FILE *f = fopen(h->path, "r");
    if (!f) return;

    /* Seek to near end */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long seek_to = sz > 4096 ? sz - 4096 : 0;
    fseek(f, seek_to, SEEK_SET);

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "## 20", 5) == 0) {
            strncpy(h->last_date, line + 3, sizeof(h->last_date) - 1);
            h->last_date[sizeof(h->last_date) - 1] = '\0';
            /* Truncate at 10 chars (YYYY-MM-DD) and strip trailing whitespace */
            h->last_date[10] = '\0';
        }
    }
    fclose(f);
}

/* Create HISTORY.md with a header block if it does not exist. */
static void ensure_file(history_t *h) {
    FILE *f = fopen(h->path, "r");
    if (f) { fclose(f); return; }

    f = fopen(h->path, "w");
    if (!f) { LOG_WARN("Cannot create HISTORY.md at %s", h->path); return; }
    fprintf(f,
        "\n\n"
        "# History\n\n"
        "Chronological, append-only log of significant agent events.\n\n"
        "Purpose: auditability, debugging, execution traceability.\n\n"
        "**This file must never be rewritten or reordered.**\n\n"
        "---\n\n");
    fclose(f);
}

/* Public API */

history_t *history_create(const char *path) {
    history_t *h = calloc(1, sizeof(history_t));
    if (!h) return NULL;
    h->path = strdup(path ? path : "HISTORY.md");
    pthread_mutex_init(&h->mu, NULL);
    ensure_file(h);
    detect_last_date(h);
    return h;
}

void history_log(history_t *h,
                       history_category_t category,
                       const char *title,
                       const char *details) {
    if (!h || !title) return;

    const char *cat_name = (category >= 0 &&
                            category < (int)(sizeof(CATEGORY_NAMES)/sizeof(*CATEGORY_NAMES)))
                         ? CATEGORY_NAMES[category] : "Event";

    char date[16], ts[32];
    get_date(date);
    get_timestamp(ts);

    pthread_mutex_lock(&h->mu);

    FILE *f = fopen(h->path, "a");
    if (!f) {
        pthread_mutex_unlock(&h->mu);
        return;
    }

    /* Insert date heading when the date changes */
    if (strcmp(date, h->last_date) != 0) {
        fprintf(f, "\n## %s\n\n", date);
        strncpy(h->last_date, date, sizeof(h->last_date) - 1);
    }

    fprintf(f, "### [%s] %s\n", cat_name, title);
    fprintf(f, "- Time: %s\n", ts);

    if (details && *details) {
        /* Write each line of details as a bullet point */
        const char *p = details;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > 0) {
                /* Already a bullet? Keep it; otherwise add "- " */
                if (p[0] == '-' || p[0] == '*')
                    fprintf(f, "%.*s\n", (int)len, p);
                else
                    fprintf(f, "- %.*s\n", (int)len, p);
            }
            p += len + (nl ? 1 : 0);
        }
    }

    fprintf(f, "\n");
    fclose(f);

    pthread_mutex_unlock(&h->mu);
}

void history_destroy(history_t *h) {
    if (!h) return;
    pthread_mutex_destroy(&h->mu);
    free(h->path);
    free(h);
}
