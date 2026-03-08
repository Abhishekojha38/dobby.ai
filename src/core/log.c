/*
 * log.c — Logging subsystem implementation
 */
#define _GNU_SOURCE
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static log_level_t g_min_level = LOG_WARN;
static FILE             *g_log_file  = NULL;
static pthread_mutex_t   g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ANSI color codes */
static const char *level_colors[] = {
    "\033[36m",  /* DEBUG: cyan   */
    "\033[32m",  /* INFO:  green  */
    "\033[33m",  /* WARN:  yellow */
    "\033[31m",  /* ERROR: red    */
};

static const char *level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

void log_init(log_level_t min_level, const char *log_file) {
    g_min_level = min_level;
    if (log_file) {
        g_log_file = fopen(log_file, "a");
    }
}

void log_shutdown(void) {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void log_set_level(log_level_t level) {
    g_min_level = level;
}

void log_write(log_level_t level, const char *file,
                     int line, const char *fmt, ...) {
    if (level < g_min_level) return;

    /* Get timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_buf);

    /* Extract basename from file path */
    const char *basename = strrchr(file, '/');
    basename = basename ? basename + 1 : file;

    va_list args;
    va_start(args, fmt);

    pthread_mutex_lock(&g_log_mutex);

    /* Write to stderr with colors if it's a terminal */
    int use_color = isatty(STDERR_FILENO);
    if (use_color) {
        fprintf(stderr, "%s[%s %s]%s %s:%d: ",
                level_colors[level], timebuf, level_names[level],
                "\033[0m", basename, line);
    } else {
        fprintf(stderr, "[%s %s] %s:%d: ",
                timebuf, level_names[level], basename, line);
    }
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");

    /* Also write to log file if configured */
    if (g_log_file) {
        fprintf(g_log_file, "[%s %s] %s:%d: ",
                timebuf, level_names[level], basename, line);
        va_end(args);
        va_start(args, fmt);
        vfprintf(g_log_file, fmt, args);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }

    pthread_mutex_unlock(&g_log_mutex);
    va_end(args);
}
