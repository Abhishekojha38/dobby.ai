/*
 * test_history.c — Unit tests for src/history/history.c
 *
 * Tests: create, log entries, date sections, concurrent safety, edge cases.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>

#include "history.h"

/* ── Test framework ──────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { printf("  %-60s", name); fflush(stdout); } while(0)
#define PASS() \
    do { printf("\033[32mPASS\033[0m\n"); g_pass++; } while(0)
#define FAIL(msg) \
    do { printf("\033[31mFAIL\033[0m  %s\n", msg); g_fail++; } while(0)
#define EXPECT_TRUE(e)  do { if (e)  PASS(); else FAIL(#e " was false"); } while(0)
#define EXPECT_FALSE(e) do { if (!(e)) PASS(); else FAIL(#e " was true"); } while(0)
#define EXPECT_NULL(e)  do { if ((e)==NULL) PASS(); else FAIL("expected NULL"); } while(0)

/* Read file into heap string; caller frees */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ── Tests ───────────────────────────────────────────────────────── */

static void test_create_and_header(void) {
    printf("\nhistory_create — file creation\n");

    char path[] = "/tmp/dobby_test_history_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) { close(fd); unlink(path); } /* start fresh */

    history_t *h = history_create(path);

    TEST("create returns non-NULL");
    EXPECT_TRUE(h != NULL);

    TEST("file exists after create");
    EXPECT_TRUE(access(path, F_OK) == 0);

    char *content = slurp(path);
    TEST("file has content (header)");
    EXPECT_TRUE(content && strlen(content) > 0);

    TEST("file contains 'HISTORY'");
    EXPECT_TRUE(content && strstr(content, "HISTORY") != NULL);

    free(content);
    history_destroy(h);
    unlink(path);
}

static void test_log_entry(void) {
    printf("\nhistory_log — entry format\n");

    char path[] = "/tmp/dobby_test_hist_log_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) { close(fd); unlink(path); }

    history_t *h = history_create(path);

    history_log(h, HISTORY_SYSTEM, "Agent Started",
                "model: mistral-nemo\nbackend: ollama");

    history_log(h, HISTORY_TOOL_EXEC, "Shell Tool Executed",
                "command: ls -la\nstatus: 0");

    history_log(h, HISTORY_ERROR, "Connection Failed", "endpoint: http://localhost:11434");

    history_destroy(h);

    char *content = slurp(path);

    TEST("entry title appears in file");
    EXPECT_TRUE(content && strstr(content, "Agent Started") != NULL);

    TEST("second entry appears");
    EXPECT_TRUE(content && strstr(content, "Shell Tool Executed") != NULL);

    TEST("error entry appears");
    EXPECT_TRUE(content && strstr(content, "Connection Failed") != NULL);

    TEST("details included in entry");
    EXPECT_TRUE(content && strstr(content, "mistral-nemo") != NULL);

    TEST("date section header present");
    EXPECT_TRUE(content && strstr(content, "##") != NULL);

    TEST("entry uses markdown heading");
    EXPECT_TRUE(content && strstr(content, "###") != NULL);

    free(content);
    unlink(path);
}

static void test_null_details(void) {
    printf("\nhistory_log — NULL details\n");

    char path[] = "/tmp/dobby_test_hist_null_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) { close(fd); unlink(path); }

    history_t *h = history_create(path);

    /* Should not crash with NULL details */
    history_log(h, HISTORY_SYSTEM, "No Details Entry", NULL);
    history_log(h, HISTORY_ERROR,  "Another No Details", NULL);

    char *content = slurp(path);
    TEST("NULL details: no crash and title present");
    EXPECT_TRUE(content && strstr(content, "No Details Entry") != NULL);

    free(content);
    history_destroy(h);
    unlink(path);
}

static void test_append_across_open(void) {
    printf("\nhistory_log — appends across reopen\n");

    char path[] = "/tmp/dobby_test_hist_append_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) { close(fd); unlink(path); }

    {
        history_t *h = history_create(path);
        history_log(h, HISTORY_SYSTEM, "First Entry", "session 1");
        history_destroy(h);
    }

    {
        history_t *h = history_create(path);
        history_log(h, HISTORY_SYSTEM, "Second Entry", "session 2");
        history_destroy(h);
    }

    char *content = slurp(path);
    TEST("first session entry preserved");
    EXPECT_TRUE(content && strstr(content, "First Entry") != NULL);

    TEST("second session entry appended");
    EXPECT_TRUE(content && strstr(content, "Second Entry") != NULL);

    free(content);
    unlink(path);
}

/* Thread worker for concurrent test */
static void *concurrent_worker(void *arg) {
    history_t *h = (history_t *)arg;
    for (int i = 0; i < 20; i++) {
        char title[64], detail[64];
        snprintf(title,  sizeof(title),  "Entry-%lu-%d", (unsigned long)pthread_self(), i);
        snprintf(detail, sizeof(detail), "thread=%lu i=%d", (unsigned long)pthread_self(), i);
        history_log(h, HISTORY_TOOL_EXEC, title, detail);
    }
    return NULL;
}

static void test_concurrent_writes(void) {
    printf("\nhistory_log — concurrent thread safety\n");

    char path[] = "/tmp/dobby_test_hist_conc_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) { close(fd); unlink(path); }

    history_t *h = history_create(path);

    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&threads[i], NULL, concurrent_worker, h);
    for (int i = 0; i < 4; i++)
        pthread_join(threads[i], NULL);

    history_destroy(h);

    /* File should exist and have all 80 entries (4 threads × 20 each) */
    char *content = slurp(path);
    TEST("concurrent writes: file exists and non-empty");
    EXPECT_TRUE(content && strlen(content) > 0);

    /* Count "###" occurrences (one per entry; header may also have one) */
    int count = 0;
    const char *p = content;
    while ((p = strstr(p, "###")) != NULL) { count++; p += 3; }

    /* 80 entries + possibly the header — just check ≥ 80 */
    TEST("concurrent writes: all 80 entries written");
    EXPECT_TRUE(count >= 80);

    free(content);
    unlink(path);
}

static void test_all_categories(void) {
    printf("\nhistory_log — all category types\n");

    char path[] = "/tmp/dobby_test_hist_cats_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) { close(fd); unlink(path); }

    history_t *h = history_create(path);

    history_log(h, HISTORY_SYSTEM,    "sys",     NULL);
    history_log(h, HISTORY_TOOL_EXEC, "tool",    NULL);
    history_log(h, HISTORY_MEMORY,    "mem",     NULL);
    history_log(h, HISTORY_MODEL,     "model",   NULL);
    history_log(h, HISTORY_API,       "api",     NULL);
    history_log(h, HISTORY_ERROR,     "error",   NULL);
    history_log(h, HISTORY_FEATURE,   "feature", NULL);
    history_log(h, HISTORY_FIX,       "fix",     NULL);

    history_destroy(h);

    char *content = slurp(path);
    TEST("all 8 category titles present in file");
    bool ok = content
        && strstr(content, "sys")
        && strstr(content, "tool")
        && strstr(content, "mem")
        && strstr(content, "model")
        && strstr(content, "api")
        && strstr(content, "error")
        && strstr(content, "feature")
        && strstr(content, "fix");
    EXPECT_TRUE(ok);

    free(content);
    unlink(path);
}

static void test_null_safety(void) {
    printf("\nNull-safety\n");

    TEST("history_create NULL path: returns non-NULL or NULL without crash");
    history_t *h = history_create(NULL);
    /* Either returns NULL or a no-op logger — both are acceptable */
    PASS(); /* no crash = pass */

    if (h) {
        /* Should not crash */
        history_log(h, HISTORY_SYSTEM, "test", NULL);
        history_destroy(h);
    }

    TEST("history_log NULL h: no crash");
    history_log(NULL, HISTORY_SYSTEM, "title", "details");
    PASS();

    TEST("history_destroy NULL: no crash");
    history_destroy(NULL);
    PASS();
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("━━━ Dobby History — Unit Tests ━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    test_create_and_header();
    test_log_entry();
    test_null_details();
    test_append_across_open();
    test_concurrent_writes();
    test_all_categories();
    test_null_safety();

    printf("\n━━━ Results: \033[32m%d passed\033[0m", g_pass);
    if (g_fail > 0) printf(", \033[31m%d FAILED\033[0m", g_fail);
    printf(" ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    return g_fail > 0 ? 1 : 0;
}
