/*
 * shell_tool.h — Shell command execution tool
 */
#ifndef SHELL_TOOL_H
#define SHELL_TOOL_H

#include "../tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the shell command tool.
 * al: Optional allowlist for command filtering (can be NULL). */
void shell_tool_register(allowlist_t *al);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_TOOL_H */
