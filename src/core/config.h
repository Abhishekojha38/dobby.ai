/*
 * config.h — Configuration loader for Dobby AI Agent
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct config config_t;

/* Load config from an INI-style file. Returns NULL on failure. */
config_t *config_load(const char *path);

/* Create empty config. */
config_t *config_create(void);

/* Get a value. Returns default_val if key is not found. */
const char *config_get(const config_t *cfg,
                             const char *section,
                             const char *key,
                             const char *default_val);

/* Get an integer value. */
int config_get_int(const config_t *cfg,
                         const char *section,
                         const char *key,
                         int default_val);

/* Get a boolean value (true/false, yes/no, 1/0). */
bool config_get_bool(const config_t *cfg,
                           const char *section,
                           const char *key,
                           bool default_val);

/* Set a value. */
void config_set(config_t *cfg,
                      const char *section,
                      const char *key,
                      const char *value);

/* Free config. */
void config_free(config_t *cfg);

/*
 * env_load — load KEY=VALUE pairs from a .env file into the process
 *                  environment using setenv(key, val, 0).
 *
 * Shell-exported variables are never overwritten (priority: shell > .env).
 * Supports: comments (#/;), blank lines, 'export KEY=VALUE', quoted values.
 * Returns the number of variables set (0 if file not found, which is fine).
 */
int env_load(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
