/*
 * file_tool.c — File system operations tool
 *
 * Provides: file_read, file_write, file_list, file_search, file_delete
 */
#define _GNU_SOURCE
#include "file_tool.h"
#include "../../core/dobby.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILE_SIZE (10 * 1024 * 1024)  /* 10 MB */

static allowlist_t *g_file_allowlist = NULL;

static bool check_path(const char *path) {
    if (g_file_allowlist &&
        allowlist_is_enabled(g_file_allowlist, ACL_PATH)) {
        if (!allowlist_check(g_file_allowlist, ACL_PATH, path)) {
            return false;
        }
    }
    return true;
}

/* file_read */

static char *file_read_exec(const cJSON *args, void *user_data) {
    (void)user_data;
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "path"));
    if (!path) return strdup("{\"error\": \"'path' required\"}");
    if (!check_path(path)) return strdup("{\"error\": \"Path not allowed\"}");

    struct stat st;
    if (stat(path, &st) != 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "error", strerror(errno));
        char *j = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        return j;
    }

    if (st.st_size > MAX_FILE_SIZE) {
        return strdup("{\"error\": \"File exceeds 10MB size limit\"}");
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "error", strerror(errno));
        char *j = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        return j;
    }

    char *content = malloc((size_t)st.st_size + 1);
    size_t read_len = fread(content, 1, (size_t)st.st_size, f);
    content[read_len] = '\0';
    fclose(f);

    /* Sanitize: strip ANSI escapes and control chars.
     * Log files and device pseudo-files (e.g. /proc, /sys) can contain
     * terminal control sequences that cause upstream JSON parse failures. */
    char *clean = sanitize_output(content);
    free(content);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "path", path);
    cJSON_AddNumberToObject(result, "size", st.st_size);
    cJSON_AddStringToObject(result, "content", clean);
    free(clean);

    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

/* file_write */

static char *file_write_exec(const cJSON *args, void *user_data) {
    (void)user_data;
    const char *path    = cJSON_GetStringValue(cJSON_GetObjectItem(args, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(args, "content"));
    if (!path || !content) return strdup("{\"error\": \"'path' and 'content' required\"}");
    if (!check_path(path)) return strdup("{\"error\": \"Path not allowed\"}");

    if (strlen(content) > MAX_FILE_SIZE) {
        return strdup("{\"error\": \"Content exceeds 10MB size limit\"}");
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "error", strerror(errno));
        char *j = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        return j;
    }

    size_t written = fwrite(content, 1, strlen(content), f);
    fclose(f);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "written");
    cJSON_AddStringToObject(result, "path", path);
    cJSON_AddNumberToObject(result, "bytes_written", (double)written);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

/* file_list */

static char *file_list_exec(const cJSON *args, void *user_data) {
    (void)user_data;
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "path"));
    if (!path) path = ".";
    if (!check_path(path)) return strdup("{\"error\": \"Path not allowed\"}");

    DIR *dir = opendir(path);
    if (!dir) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "error", strerror(errno));
        char *j = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        return j;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry->d_name);

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            cJSON_AddStringToObject(item, "type",
                                     S_ISDIR(st.st_mode) ? "directory" : "file");
            if (!S_ISDIR(st.st_mode)) {
                cJSON_AddNumberToObject(item, "size", (double)st.st_size);
            }
        }

        cJSON_AddItemToArray(entries, item);
    }
    closedir(dir);

    cJSON_AddStringToObject(result, "path", path);
    cJSON_AddItemToObject(result, "entries", entries);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

/* file_search */

static void search_recursive(const char *dir_path, const char *pattern,
                              cJSON *results, int depth) {
    if (depth > 5) return; /* Max recursion depth */

    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            search_recursive(full_path, pattern, results, depth + 1);
        } else if (strcasestr(entry->d_name, pattern)) {
            cJSON_AddItemToArray(results, cJSON_CreateString(full_path));
        }
    }
    closedir(dir);
}

static char *file_search_exec(const cJSON *args, void *user_data) {
    (void)user_data;
    const char *path    = cJSON_GetStringValue(cJSON_GetObjectItem(args, "path"));
    const char *pattern = cJSON_GetStringValue(cJSON_GetObjectItem(args, "pattern"));
    if (!pattern) return strdup("{\"error\": \"'pattern' required\"}");
    if (!path) path = ".";
    if (!check_path(path)) return strdup("{\"error\": \"Path not allowed\"}");

    cJSON *result  = cJSON_CreateObject();
    cJSON *matches = cJSON_CreateArray();
    search_recursive(path, pattern, matches, 0);

    cJSON_AddStringToObject(result, "pattern", pattern);
    cJSON_AddItemToObject(result, "matches", matches);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

/* file_delete */

static char *file_delete_exec(const cJSON *args, void *user_data) {
    (void)user_data;
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "path"));
    if (!path) return strdup("{\"error\": \"'path' required\"}");
    if (!check_path(path)) return strdup("{\"error\": \"Path not allowed\"}");

    if (unlink(path) != 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "error", strerror(errno));
        char *j = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        return j;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "deleted");
    cJSON_AddStringToObject(result, "path", path);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

/* Registration */

void file_tool_register(allowlist_t *al) {
    g_file_allowlist = al;

    tool_t read_tool = {
        .name = "file_read",
        .description = "Read the contents of a file",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Path to the file\"}"
            "},\"required\":[\"path\"]}",
        .execute = file_read_exec, .user_data = NULL,
    };
    tool_register(&read_tool);

    tool_t write_tool = {
        .name = "file_write",
        .description = "Write content to a file (creates or overwrites)",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Path to the file\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
            "},\"required\":[\"path\",\"content\"]}",
        .execute = file_write_exec, .user_data = NULL,
    };
    tool_register(&write_tool);

    tool_t list_tool = {
        .name = "file_list",
        .description = "List files and directories in a path",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Directory path (default: current dir)\"}"
            "},\"required\":[]}",
        .execute = file_list_exec, .user_data = NULL,
    };
    tool_register(&list_tool);

    tool_t search_tool = {
        .name = "file_search",
        .description = "Search for files by name pattern (recursive)",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Directory to search in\"},"
            "\"pattern\":{\"type\":\"string\",\"description\":\"Filename pattern to search for\"}"
            "},\"required\":[\"pattern\"]}",
        .execute = file_search_exec, .user_data = NULL,
    };
    tool_register(&search_tool);

    tool_t delete_tool = {
        .name = "file_delete",
        .description = "Delete a file",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Path to the file to delete\"}"
            "},\"required\":[\"path\"]}",
        .execute = file_delete_exec, .user_data = NULL,
    };
    tool_register(&delete_tool);

    LOG_DEBUG("File tools registered (5 tools)");
}
