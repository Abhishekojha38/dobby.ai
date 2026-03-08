/*
 * file_tool.h — File operations tool
 */
#ifndef FILE_TOOL_H
#define FILE_TOOL_H

#include "../tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register file operation tools (read, write, list, search, delete).
 * al: Optional allowlist for path filtering. */
void file_tool_register(allowlist_t *al);

#ifdef __cplusplus
}
#endif

#endif /* FILE_TOOL_H */
