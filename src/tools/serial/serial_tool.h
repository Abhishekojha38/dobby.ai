/*
 * serial_tool.h — Native serial port tool for Dobby AI Agent
 *
 * Provides the `serial_exec` tool: connect to a serial device, optionally
 * log in, run one or more commands, return structured JSON results.
 *
 * Device configuration is read from a device registry file
 * (default: <workspace>/serial/device.conf).  The LLM only needs to pass
 * the device name; all port parameters are resolved internally.
 *
 * Tool call schema (either form is accepted):
 *
 *   Named device (preferred):
 *     { "device": "m1700_0", "commands": ["uname -a", "df -h"] }
 *
 *   Explicit parameters (one-off / override):
 *     { "port": "/dev/ttyUSB0", "baud": 115200,
 *       "commands": ["uname -a"],
 *       "no_login": true, "prompt": "# " }
 *
 *   Mixed (device sets defaults, explicit fields override):
 *     { "device": "m1700_0", "commands": ["uname -a"], "timeout": 30 }
 *
 * JSON result schema:
 *   {
 *     "status":       "ok" | "error" | "timeout" | "login_failed",
 *     "device":       "m1700_0"          (if named device was used),
 *     "port":         "/dev/ttyUSB0",
 *     "baud":         115200,
 *     "commands":     ["uname -a"],
 *     "login_status": "ok" | "skipped" | "already_authenticated",
 *     "results": [
 *       { "command": "uname -a", "output": "Linux ...", "status": "ok" }
 *     ],
 *     "error":        "human-readable message  (only when status != ok)",
 *     "elapsed_ms":   1234
 *   }
 */
#ifndef SERIAL_TOOL_H
#define SERIAL_TOOL_H

#include "../tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register the serial_exec tool.
 *
 * device_conf_path: path to the device registry INI file.
 *   Pass NULL to use the default location
 *   (<workspace>/serial/device.conf).
 *   The path is stored; the file is re-read on each tool call so
 *   changes to device.conf take effect without restarting Dobby.
 */
void serial_tool_register(const char *device_conf_path);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_TOOL_H */
