/*
 * test_config.c — Unit tests for src/core/config.c
 *
 * Tests: load, get, get_int, get_bool, set, env_load, edge cases.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "config.h"

/* ── Test framework ──────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { printf("  %-60s", name); fflush(stdout); } while(0)
#define PASS() \
    do { printf("\033[32mPASS\033[0m\n"); g_pass++; } while(0)
#define FAIL(msg) \
    do { printf("\033[31mFAIL\033[0m  %s\n", msg); g_fail++; } while(0)
#define EXPECT_STR(got, want) do { \
    const char *_g = (got), *_w = (want); \
    if (_g && _w && strcmp(_g, _w) == 0) PASS(); \
    else { char _m[256]; snprintf(_m,sizeof(_m),"got='%s' want='%s'",_g?_g:"NULL",_w); FAIL(_m); } \
} while(0)
#define EXPECT_INT(got, want) do { \
    int _g=(int)(got),_w=(int)(want); \
    if (_g==_w) PASS(); \
    else { char _m[64]; snprintf(_m,sizeof(_m),"got=%d want=%d",_g,_w); FAIL(_m); } \
} while(0)
#define EXPECT_TRUE(e)  do { if (e)  PASS(); else FAIL(#e " was false"); } while(0)
#define EXPECT_FALSE(e) do { if (!(e)) PASS(); else FAIL(#e " was true"); } while(0)
#define EXPECT_NULL(e)  do { if ((e)==NULL) PASS(); else FAIL("expected NULL"); } while(0)

/* ── Write temp INI file ─────────────────────────────────────────── */

static char *write_tmp(const char *content) {
    char *path = strdup("/tmp/dobby_test_cfg_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    write(fd, content, strlen(content));
    close(fd);
    return path;
}

/* ── Tests ───────────────────────────────────────────────────────── */

static void test_basic_load(void) {
    printf("\nconfig_load — basic INI parsing\n");

    char *path = write_tmp(
        "[provider]\n"
        "type  = ollama\n"
        "model = mistral-nemo:12b\n"
        "\n"
        "[gateway]\n"
        "port = 8080\n"
        "\n"
        "[agent]\n"
        "max_iterations = 10\n"
        "debug = true\n"
    );
    if (!path) { printf("  SKIP (tmpfile failed)\n"); return; }

    config_t *cfg = config_load(path);
    TEST("load returns non-NULL");
    EXPECT_TRUE(cfg != NULL);

    TEST("string value: type");
    EXPECT_STR(config_get(cfg, "provider", "type", ""), "ollama");

    TEST("string value: model");
    EXPECT_STR(config_get(cfg, "provider", "model", ""), "mistral-nemo:12b");

    TEST("int value: port");
    EXPECT_INT(config_get_int(cfg, "gateway", "port", 0), 8080);

    TEST("int value: max_iterations");
    EXPECT_INT(config_get_int(cfg, "agent", "max_iterations", 0), 10);

    TEST("bool value: debug=true");
    EXPECT_TRUE(config_get_bool(cfg, "agent", "debug", false));

    TEST("missing key returns default");
    EXPECT_STR(config_get(cfg, "provider", "missing_key", "default"), "default");

    TEST("missing section returns default");
    EXPECT_STR(config_get(cfg, "nosection", "key", "fallback"), "fallback");

    config_free(cfg);
    unlink(path);
    free(path);
}

static void test_comments_and_blanks(void) {
    printf("\nComment and blank line handling\n");

    char *path = write_tmp(
        "# Top-level comment\n"
        "\n"
        "[section]\n"
        "# inline comment section\n"
        "key1 = value1  # trailing comment\n"
        "key2 = value2\n"
        "; semicolon comment\n"
        "key3 = hello world\n"
        "\n"
    );
    if (!path) { printf("  SKIP\n"); return; }

    config_t *cfg = config_load(path);

    TEST("key1 parsed despite trailing comment");
    /* Implementations may or may not strip trailing comments — just check non-empty */
    const char *v1 = config_get(cfg, "section", "key1", NULL);
    EXPECT_TRUE(v1 && strlen(v1) > 0);

    TEST("key3 with spaces in value");
    const char *v3 = config_get(cfg, "section", "key3", NULL);
    EXPECT_TRUE(v3 && strncmp(v3, "hello", 5) == 0);

    config_free(cfg);
    unlink(path);
    free(path);
}

static void test_bool_variants(void) {
    printf("\nBoolean value variants\n");

    char *path = write_tmp(
        "[flags]\n"
        "a = true\n"
        "b = yes\n"
        "c = 1\n"
        "d = false\n"
        "e = no\n"
        "f = 0\n"
    );
    if (!path) { printf("  SKIP\n"); return; }

    config_t *cfg = config_load(path);

    TEST("true parses as true");  EXPECT_TRUE(config_get_bool(cfg, "flags", "a", false));
    TEST("yes parses as true");   EXPECT_TRUE(config_get_bool(cfg, "flags", "b", false));
    TEST("1 parses as true");     EXPECT_TRUE(config_get_bool(cfg, "flags", "c", false));
    TEST("false parses as false");EXPECT_FALSE(config_get_bool(cfg, "flags", "d", true));
    TEST("no parses as false");   EXPECT_FALSE(config_get_bool(cfg, "flags", "e", true));
    TEST("0 parses as false");    EXPECT_FALSE(config_get_bool(cfg, "flags", "f", true));

    config_free(cfg);
    unlink(path);
    free(path);
}

static void test_config_set(void) {
    printf("\nconfig_set (programmatic override)\n");

    config_t *cfg = config_create();

    config_set(cfg, "section", "key", "value");
    TEST("set then get");
    EXPECT_STR(config_get(cfg, "section", "key", ""), "value");

    config_set(cfg, "section", "key", "updated");
    TEST("overwrite with set");
    EXPECT_STR(config_get(cfg, "section", "key", ""), "updated");

    config_set(cfg, "other", "num", "42");
    TEST("int from set value");
    EXPECT_INT(config_get_int(cfg, "other", "num", 0), 42);

    config_free(cfg);
}

static void test_missing_file(void) {
    printf("\nconfig_load — missing file\n");
    config_t *cfg = config_load("/tmp/dobby_no_such_config_XXXXXXXX.conf");
    TEST("returns NULL for missing file");
    EXPECT_NULL(cfg);
}

static void test_env_load(void) {
    printf("\nenv_load — .env file parsing\n");

    char *path = write_tmp(
        "# env file\n"
        "API_KEY=test-key-abc123\n"
        "MODEL=llama-3.3-70b\n"
        "export PORT=9090\n"
        "EMPTY=\n"
        "\n"
        "; another comment style\n"
        "WITH_SPACES = spaced value\n"
    );
    if (!path) { printf("  SKIP\n"); return; }

    /* Unset first so we start clean */
    unsetenv("API_KEY");
    unsetenv("MODEL");
    unsetenv("PORT");
    unsetenv("EMPTY");
    unsetenv("WITH_SPACES");

    int n = env_load(path);

    TEST("env_load returns count > 0");
    EXPECT_TRUE(n > 0);

    TEST("API_KEY loaded");
    const char *key = getenv("API_KEY");
    EXPECT_TRUE(key && strcmp(key, "test-key-abc123") == 0);

    TEST("MODEL loaded");
    const char *model = getenv("MODEL");
    EXPECT_TRUE(model && strcmp(model, "llama-3.3-70b") == 0);

    TEST("export prefix handled: PORT loaded");
    const char *port = getenv("PORT");
    EXPECT_TRUE(port && strcmp(port, "9090") == 0);

    /* Shell-set vars must not be overwritten */
    setenv("SHELL_SET", "original", 1);
    /* Write file with same key */
    char *path2 = write_tmp("SHELL_SET=overwritten\n");
    if (path2) {
        env_load(path2);
        TEST("pre-set shell var not overwritten by env_load");
        EXPECT_STR(getenv("SHELL_SET"), "original");
        unlink(path2);
        free(path2);
    }

    unlink(path);
    free(path);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("━━━ Dobby Config — Unit Tests ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    test_basic_load();
    test_comments_and_blanks();
    test_bool_variants();
    test_config_set();
    test_missing_file();
    test_env_load();

    printf("\n━━━ Results: \033[32m%d passed\033[0m", g_pass);
    if (g_fail > 0) printf(", \033[31m%d FAILED\033[0m", g_fail);
    printf(" ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    return g_fail > 0 ? 1 : 0;
}
