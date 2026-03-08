/*
 * allowlist.c — Security allowlist implementation
 */
#define _GNU_SOURCE
#include "allowlist.h"
#include "../core/log.h"

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_RULES 256

typedef struct {
    char *pattern;
} acl_rule_t;

typedef struct {
    acl_rule_t rules[MAX_RULES];
    int        count;
    bool       enabled;
} acl_list_t;

struct allowlist {
    acl_list_t lists[4]; /* COMMAND, PATH, ENDPOINT, EMAIL */
};

static const char *type_names[] = { "command", "path", "endpoint", "email" };

allowlist_t *allowlist_create(void) {
    allowlist_t *al = calloc(1, sizeof(allowlist_t));
    /* Disabled by default — allows everything */
    for (int i = 0; i < 4; i++) {
        al->lists[i].enabled = false;
    }
    LOG_DEBUG("Allowlist system created");
    return al;
}

result_t allowlist_add(allowlist_t *al, acl_type_t type,
                                    const char *pattern) {
    acl_list_t *list = &al->lists[type];
    if (list->count >= MAX_RULES) {
        return err(ERR_GENERIC, "Max rules reached for %s",
                         type_names[type]);
    }
    list->rules[list->count].pattern = strdup(pattern);
    list->count++;
    list->enabled = true;
    LOG_DEBUG("Allowlist: added %s rule: %s", type_names[type], pattern);
    return ok();
}

result_t allowlist_load(allowlist_t *al, const char *config_path) {
    FILE *f = fopen(config_path, "r");
    if (!f) {
        LOG_DEBUG("No allowlist config at %s (allowlists disabled)", config_path);
        return ok();
    }

    char line[1024];
    acl_type_t current_type = ACL_COMMAND;

    while (fgets(line, sizeof(line), f)) {
        char *s = str_trim(line);
        if (*s == '\0' || *s == '#') continue;

        if (strcmp(s, "[commands]") == 0) { current_type = ACL_COMMAND;  continue; }
        if (strcmp(s, "[paths]") == 0)    { current_type = ACL_PATH;     continue; }
        if (strcmp(s, "[endpoints]") == 0){ current_type = ACL_ENDPOINT; continue; }
        if (strcmp(s, "[email]") == 0)    { current_type = ACL_EMAIL;    continue; }

        /* Parse "allow = val1, val2, val3" */
        if (strncmp(s, "allow", 5) == 0) {
            char *eq = strchr(s, '=');
            if (!eq) continue;
            char *vals = str_trim(eq + 1);

            /* Split by comma */
            char *token = strtok(vals, ",");
            while (token) {
                char *trimmed = str_trim(token);
                if (*trimmed) {
                    allowlist_add(al, current_type, trimmed);
                }
                token = strtok(NULL, ",");
            }
        }
    }

    fclose(f);
    LOG_DEBUG("Loaded allowlist config from %s", config_path);
    return ok();
}

bool allowlist_check(allowlist_t *al, acl_type_t type,
                           const char *value) {
    acl_list_t *list = &al->lists[type];

    /* If not enabled, allow everything */
    if (!list->enabled) return true;

    for (int i = 0; i < list->count; i++) {
        const char *pattern = list->rules[i].pattern;

        switch (type) {
        case ACL_COMMAND: {
            /* Check if command starts with the allowed pattern */
            if (fnmatch(pattern, value, 0) == 0) return true;
            /* Also check just the command name (before first space) */
            const char *space = strchr(value, ' ');
            if (space) {
                char *cmd = strndup(value, (size_t)(space - value));
                bool match = (fnmatch(pattern, cmd, 0) == 0);
                free(cmd);
                if (match) return true;
            }
            break;
        }
        case ACL_PATH:
            /* Prefix match for paths */
            if (str_starts_with(value, pattern)) return true;
            break;

        case ACL_ENDPOINT:
            /* Exact or prefix match for endpoints */
            if (str_starts_with(value, pattern)) return true;
            break;

        case ACL_EMAIL:
            /* Case-insensitive exact match or wildcard domain (*@example.com) */
            if (fnmatch(pattern, value, FNM_CASEFOLD) == 0) return true;
            break;
        }
    }

    LOG_WARN("BLOCKED %s: %s", type_names[type], value);
    return false;
}

bool allowlist_is_enabled(allowlist_t *al, acl_type_t type) {
    return al->lists[type].enabled;
}

void allowlist_set_enabled(allowlist_t *al, acl_type_t type,
                                 bool enabled) {
    al->lists[type].enabled = enabled;
}

int allowlist_rule_count(allowlist_t *al, acl_type_t type) {
    if (!al) return 0;
    return al->lists[type].count;
}

const char *allowlist_rule_pattern(allowlist_t *al, acl_type_t type, int i) {
    if (!al || i < 0 || i >= al->lists[type].count) return NULL;
    return al->lists[type].rules[i].pattern;
}

bool allowlist_remove(allowlist_t *al, acl_type_t type, const char *pattern) {
    if (!al || !pattern) return false;
    acl_list_t *list = &al->lists[type];
    for (int i = 0; i < list->count; i++) {
        if (strcasecmp(list->rules[i].pattern, pattern) == 0) {
            free(list->rules[i].pattern);
            /* Shift remaining rules down */
            for (int j = i; j < list->count - 1; j++)
                list->rules[j] = list->rules[j + 1];
            list->count--;
            if (list->count == 0) list->enabled = false;
            LOG_DEBUG("Allowlist: removed %s rule: %s", type_names[type], pattern);
            return true;
        }
    }
    return false;
}

result_t allowlist_save_email(allowlist_t *al, const char *config_path) {
    if (!al || !config_path) return err(ERR_GENERIC, "null argument");

    /* Read the whole file */
    FILE *f = fopen(config_path, "r");
    char *content = NULL;
    size_t content_len = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            content = malloc((size_t)sz + 1);
            if (content) {
                content_len = fread(content, 1, (size_t)sz, f);
                content[content_len] = '\0';
            }
        }
        fclose(f);
    }

    /* Build new [email] section */
    char email_section[4096] = {0};
    acl_list_t *list = &al->lists[ACL_EMAIL];
    if (list->count > 0) {
        strcat(email_section, "[email]\nallow = ");
        for (int i = 0; i < list->count; i++) {
            if (i > 0) strcat(email_section, ", ");
            strcat(email_section, list->rules[i].pattern);
        }
        strcat(email_section, "\n");
    }

    /* Remove existing [email] section from content */
    char *new_content = NULL;
    if (content) {
        /* Find [email] section start */
        char *email_start = strstr(content, "\n[email]");
        if (!email_start) email_start = strstr(content, "[email]");
        if (email_start) {
            /* Find next section or end */
            char *next_section = email_start + 1;
            while ((next_section = strchr(next_section, '\n')) != NULL) {
                next_section++;
                if (*next_section == '[' || *next_section == '\0') break;
                if (*next_section == '#') {
                    /* Skip comment lines within section */
                    char *after_comment = strchr(next_section, '\n');
                    if (after_comment) next_section = after_comment + 1;
                }
            }
            /* Rebuild without old [email] section */
            size_t prefix_len = (size_t)(email_start - content);
            size_t suffix_len = next_section ? strlen(next_section) : 0;
            new_content = malloc(prefix_len + strlen(email_section) + suffix_len + 4);
            memcpy(new_content, content, prefix_len);
            new_content[prefix_len] = '\0';
            strcat(new_content, "\n");
            strcat(new_content, email_section);
            if (next_section && *next_section) strcat(new_content, next_section);
        }
    }

    /* Write back */
    f = fopen(config_path, "w");
    if (!f) {
        free(content); free(new_content);
        return err(ERR_GENERIC, "cannot write allowlist.conf");
    }
    if (new_content) {
        fputs(new_content, f);
    } else if (content) {
        fputs(content, f);
        if (email_section[0]) fprintf(f, "\n%s", email_section);
    } else {
        fprintf(f, "# Dobby Security Allowlists\n\n%s", email_section);
    }
    fclose(f);
    free(content);
    free(new_content);
    LOG_DEBUG("Allowlist: saved email rules to %s", config_path);
    return ok();
}

void allowlist_destroy(allowlist_t *al) {
    if (!al) return;
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < al->lists[t].count; i++) {
            free(al->lists[t].rules[i].pattern);
        }
    }
    free(al);
}
