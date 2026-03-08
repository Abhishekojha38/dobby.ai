/*
 * tool_registry.c — Tool registry implementation
 */
#include "tool_registry.h"

#include <stdlib.h>
#include <string.h>

static tool_t g_tools[MAX_TOOLS];
static int          g_tool_count = 0;

void tool_registry_init(void) {
    g_tool_count = 0;
    memset(g_tools, 0, sizeof(g_tools));
    LOG_DEBUG("Tool registry initialized");
}

result_t tool_register(const tool_t *tool) {
    if (g_tool_count >= MAX_TOOLS) {
        return err(ERR_GENERIC, "Max tools reached (%d)",
                         MAX_TOOLS);
    }
    if (!tool->name || !tool->execute) {
        return err(ERR_GENERIC, "Tool must have name and execute fn");
    }

    g_tools[g_tool_count] = *tool;
    g_tool_count++;
    LOG_DEBUG("Registered tool: %s", tool->name);
    return ok();
}

const tool_t *tool_find(const char *name) {
    for (int i = 0; i < g_tool_count; i++) {
        if (strcmp(g_tools[i].name, name) == 0) {
            return &g_tools[i];
        }
    }
    return NULL;
}

int tool_count(void) {
    return g_tool_count;
}

const tool_t *tool_get(int index) {
    if (index < 0 || index >= g_tool_count) return NULL;
    return &g_tools[index];
}

char *tool_generate_json(void) {
    cJSON *tools_array = cJSON_CreateArray();

    for (int i = 0; i < g_tool_count; i++) {
        cJSON *tool_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_obj, "type", "function");

        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", g_tools[i].name);
        cJSON_AddStringToObject(func, "description",
                                g_tools[i].description ? g_tools[i].description : "");

        /* Parse parameters schema from the stored JSON string */
        if (g_tools[i].parameters_schema) {
            cJSON *params = cJSON_Parse(g_tools[i].parameters_schema);
            if (params) {
                cJSON_AddItemToObject(func, "parameters", params);
            }
        }

        cJSON_AddItemToObject(tool_obj, "function", func);
        cJSON_AddItemToArray(tools_array, tool_obj);
    }

    char *json_str = cJSON_PrintUnformatted(tools_array);
    cJSON_Delete(tools_array);
    return json_str;
}

void tool_registry_shutdown(void) {
    g_tool_count = 0;
    LOG_DEBUG("Tool registry shutdown");
}
