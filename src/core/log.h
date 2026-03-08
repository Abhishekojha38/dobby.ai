/*
 * log.h — Logging subsystem for Dobby AI Agent
 */
#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} log_level_t;

/* Initialize logging. Pass NULL for log_file to log to stderr only. */
void log_init(log_level_t min_level, const char *log_file);

/* Shutdown logging, close file handle if any. */
void log_shutdown(void);

/* Set minimum log level at runtime. */
void log_set_level(log_level_t level);

/* Core log function — use the macros below instead. */
void log_write(log_level_t level, const char *file,
                     int line, const char *fmt, ...);

#define LOG_DEBUG(...) log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* LOG_H */
