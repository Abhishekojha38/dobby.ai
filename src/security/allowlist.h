/*
 * allowlist.h — Security allowlists for commands, paths, endpoints
 */
#ifndef ALLOWLIST_H
#define ALLOWLIST_H

#include "../core/dobby.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACL_COMMAND,
    ACL_PATH,
    ACL_ENDPOINT,
    ACL_EMAIL,       /* sender allowlist — inbound receive + outbound send */
} acl_type_t;

typedef struct allowlist allowlist_t;

/* Create the allowlist system. */
allowlist_t *allowlist_create(void);

/* Add an allowed pattern. Supports glob patterns for paths.
 * For commands: exact match or prefix (e.g., "ls", "git *")
 * For paths: prefix match (e.g., "/home/user/", "/tmp/")
 * For endpoints: exact URL or domain match */
result_t allowlist_add(allowlist_t *al, acl_type_t type,
                                    const char *pattern);

/* Load allowlists from config file (INI-style).
 * [commands]
 * allow = ls, cat, grep, git
 * [paths]
 * allow = /home/user, /tmp
 * [endpoints]
 * allow = http://localhost:11434 */
result_t allowlist_load(allowlist_t *al, const char *config_path);

/* Check if a value is allowed. Logs blocked attempts. */
bool allowlist_check(allowlist_t *al, acl_type_t type,
                           const char *value);

/* Check if allowlist enforcement is enabled (false = allow everything). */
bool allowlist_is_enabled(allowlist_t *al, acl_type_t type);

/* Enable/disable enforcement for a type. */
void allowlist_set_enabled(allowlist_t *al, acl_type_t type,
                                 bool enabled);

/* Iterate rules: returns rule count for the given type. */
int allowlist_rule_count(allowlist_t *al, acl_type_t type);

/* Get the pattern string for rule index i (0-based). Returns NULL if OOB. */
const char *allowlist_rule_pattern(allowlist_t *al, acl_type_t type, int i);

/* Remove a rule by pattern (exact match). Returns true if found and removed. */
bool allowlist_remove(allowlist_t *al, acl_type_t type, const char *pattern);

/* Save the email allowlist section back to the config file path. */
result_t allowlist_save_email(allowlist_t *al, const char *config_path);

/* Destroy the allowlist. */
void allowlist_destroy(allowlist_t *al);

#ifdef __cplusplus
}
#endif

#endif /* ALLOWLIST_H */
