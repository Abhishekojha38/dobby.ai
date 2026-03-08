/*
 * tool_registry.h — Tool registry for the agentic loop
 */
#ifndef TOOL_REGISTRY_H
#define TOOL_REGISTRY_H

#include "../core/dobby.h"
#include "../core/cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_TOOLS 64

/* Tool execute function: takes JSON args, returns JSON result string (caller frees). */
typedef char *(*tool_fn)(const cJSON *args, void *user_data);

typedef struct {
    const char   *name;
    const char   *description;
    const char   *parameters_schema;  /* JSON Schema string for parameters */
    tool_fn execute;
    void         *user_data;
} tool_t;

/* Initialize the tool registry. */
void tool_registry_init(void);

/* Register a tool. */
result_t tool_register(const tool_t *tool);

/* Find a tool by name. Returns NULL if not found. */
const tool_t *tool_find(const char *name);

/* Get number of registered tools. */
int tool_count(void);

/* Get tool by index. */
const tool_t *tool_get(int index);

/* Generate the tools array JSON for the LLM (caller frees). */
char *tool_generate_json(void);

/* Shutdown the tool registry. */
void tool_registry_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_REGISTRY_H */
