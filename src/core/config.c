/*
 * config.c — INI-style configuration loader + .env file loader
 *
 * Format (dobby.conf):
 *   [section]
 *   key = value
 *   # comment
 *
 * Format (.env):
 *   KEY=value
 *   KEY="quoted value"
 *   # comment
 *   export KEY=value    (export prefix is stripped)
 *
 * Priority (highest to lowest):
 *   1. Shell-exported environment variables
 *   2. .env file variables
 *   3. dobby.conf values
 *   4. Built-in defaults
 *
 * .env variables are loaded via setenv(key, val, 0) — the 0 means
 * they do NOT overwrite variables already set in the shell environment.
 */
#define _GNU_SOURCE
#include "config.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 256
#define MAX_KEY_LEN 128

typedef struct {
    char section[MAX_KEY_LEN];
    char key[MAX_KEY_LEN];
    char *value;
} config_entry_t;

struct config {
    config_entry_t entries[MAX_ENTRIES];
    int            count;
};

config_t *config_create(void) {
    config_t *cfg = calloc(1, sizeof(config_t));
    return cfg;
}

config_t *config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_WARN("Config file not found: %s (using defaults)", path);
        return config_create();
    }

    config_t *cfg = config_create();
    char line[1024];
    char current_section[MAX_KEY_LEN] = "general";

    while (fgets(line, sizeof(line), f)) {
        /* Trim */
        char *s = line;
        while (isspace((unsigned char)*s)) s++;

        /* Skip empty lines and comments */
        if (*s == '\0' || *s == '#' || *s == ';') continue;

        /* Section header */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(current_section, sizeof(current_section), "%s", s + 1);
            }
            continue;
        }

        /* Key = value */
        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = str_trim(s);
        char *val = str_trim(eq + 1);

        /* Remove trailing newline */
        size_t vlen = strlen(val);
        if (vlen > 0 && val[vlen - 1] == '\n') val[vlen - 1] = '\0';

        config_set(cfg, current_section, key, val);
    }

    fclose(f);
    LOG_DEBUG("Loaded config from %s (%d entries)", path, cfg->count);
    return cfg;
}

const char *config_get(const config_t *cfg,
                             const char *section,
                             const char *key,
                             const char *default_val) {
    if (!cfg) return default_val;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].section, section) == 0 &&
            strcmp(cfg->entries[i].key, key) == 0) {
            return cfg->entries[i].value;
        }
    }
    return default_val;
}

int config_get_int(const config_t *cfg,
                         const char *section,
                         const char *key,
                         int default_val) {
    const char *val = config_get(cfg, section, key, NULL);
    if (!val) return default_val;
    return atoi(val);
}

bool config_get_bool(const config_t *cfg,
                           const char *section,
                           const char *key,
                           bool default_val) {
    const char *val = config_get(cfg, section, key, NULL);
    if (!val) return default_val;
    return (strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 ||
            strcmp(val, "1") == 0);
}

void config_set(config_t *cfg,
                      const char *section,
                      const char *key,
                      const char *value) {
    if (!cfg || cfg->count >= MAX_ENTRIES) return;

    /* Check if it already exists and update */
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].section, section) == 0 &&
            strcmp(cfg->entries[i].key, key) == 0) {
            free(cfg->entries[i].value);
            cfg->entries[i].value = strdup(value);
            return;
        }
    }

    /* New entry */
    config_entry_t *e = &cfg->entries[cfg->count++];
    snprintf(e->section, sizeof(e->section), "%s", section);
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->value = strdup(value);
}

void config_free(config_t *cfg) {
    if (!cfg) return;
    for (int i = 0; i < cfg->count; i++) {
        free(cfg->entries[i].value);
    }
    free(cfg);
}

/* .env file loader */

/*
 * env_load — load KEY=VALUE pairs from a .env file into the process
 *                  environment.
 *
 * Rules:
 *   - Lines starting with '#' or ';' are comments.
 *   - Blank lines are skipped.
 *   - 'export KEY=VALUE' is accepted (the 'export ' prefix is stripped).
 *   - Values may be quoted with single or double quotes; quotes are stripped.
 *   - Inline comments (# after value) are NOT stripped — keep values clean.
 *   - setenv(key, val, 0) is used: shell-exported variables are NEVER
 *     overwritten.  This preserves the priority: shell > .env > dobby.conf.
 *
 * Returns the number of variables successfully set (0 if file not found).
 */
int env_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_DEBUG(".env file not found at '%s' (optional — skipping)", path);
        return 0;
    }

    int count = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline / carriage-return */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* Trim leading whitespace */
        char *s = line;
        while (isspace((unsigned char)*s)) s++;

        /* Skip empty lines and comments */
        if (*s == '\0' || *s == '#' || *s == ';') continue;

        /* Strip optional leading 'export ' */
        if (strncmp(s, "export ", 7) == 0) s += 7;
        while (isspace((unsigned char)*s)) s++;

        /* Find '=' separator */
        char *eq = strchr(s, '=');
        if (!eq) continue;

        /* Split into key / value */
        *eq = '\0';
        char *key = s;
        char *val = eq + 1;

        /* Trim key */
        size_t klen = strlen(key);
        while (klen > 0 && isspace((unsigned char)key[klen-1])) key[--klen] = '\0';
        if (klen == 0) continue;

        /* Trim value leading whitespace */
        while (isspace((unsigned char)*val)) val++;

        /* Strip matching outer quotes (" or ') */
        size_t vlen = strlen(val);
        if (vlen >= 2 &&
            ((val[0] == '"'  && val[vlen-1] == '"') ||
             (val[0] == '\'' && val[vlen-1] == '\''))) {
            val[vlen-1] = '\0';
            val++;
        }

        /* setenv with overwrite=0: shell env takes priority */
        if (setenv(key, val, 0) == 0) {
            LOG_DEBUG(".env: %s=%s", key, val);
            count++;
        }
    }

    fclose(f);
    LOG_DEBUG("Loaded %d variable(s) from %s", count, path);
    return count;
}
