/*
 * test_allowlist.c — Unit tests for src/security/allowlist.c
 *
 * Tests all public API functions: create, add, check, is_enabled,
 * set_enabled, rule_count, rule_pattern, remove, load, save.
 *
 * Build (via CMake):
 *   cmake -DBUILD_TESTS=ON .. && make test_allowlist
 *
 * Build (standalone):
 *   gcc -std=c17 -D_GNU_SOURCE -Wall \
 *       -I../../src -I../../src/security -I../../src/core \
 *       test_allowlist.c \
 *       ../../src/security/allowlist.c \
 *       ../../src/core/types.c ../../src/core/log.c \
 *       -o test_allowlist && ./test_allowlist
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "allowlist.h"

/* ── Test framework ──────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { printf("  %-60s", name); fflush(stdout); } while(0)

#define PASS() \
    do { printf("\033[32mPASS\033[0m\n"); g_pass++; } while(0)

#define FAIL(msg) \
    do { printf("\033[31mFAIL\033[0m  %s\n", msg); g_fail++; } while(0)

#define EXPECT_TRUE(expr) \
    do { if (expr) PASS(); else FAIL(#expr " was false"); } while(0)

#define EXPECT_FALSE(expr) \
    do { if (!(expr)) PASS(); else FAIL(#expr " was true"); } while(0)

#define EXPECT_INT(got, want) do { \
    int _g = (int)(got), _w = (int)(want); \
    if (_g == _w) PASS(); \
    else { char _m[128]; snprintf(_m,sizeof(_m),"got=%d want=%d",_g,_w); FAIL(_m); } \
} while(0)

#define EXPECT_STR(got, want) do { \
    const char *_g = (got), *_w = (want); \
    if (_g && _w && strcmp(_g, _w) == 0) PASS(); \
    else FAIL("string mismatch"); \
} while(0)

/* ── Tests ───────────────────────────────────────────────────────── */

static void test_create_disabled(void) {
    printf("\nallowlist_create — disabled by default\n");
    allowlist_t *al = allowlist_create();

    TEST("COMMAND disabled after create");
    EXPECT_FALSE(allowlist_is_enabled(al, ACL_COMMAND));

    TEST("PATH disabled after create");
    EXPECT_FALSE(allowlist_is_enabled(al, ACL_PATH));

    TEST("EMAIL disabled after create");
    EXPECT_FALSE(allowlist_is_enabled(al, ACL_EMAIL));

    TEST("ENDPOINT disabled after create");
    EXPECT_FALSE(allowlist_is_enabled(al, ACL_ENDPOINT));

    TEST("check returns true when disabled (allow-all)");
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "anyone@example.com"));

    allowlist_destroy(al);
}

static void test_add_and_check(void) {
    printf("\nallowlist_add + allowlist_check\n");
    allowlist_t *al = allowlist_create();

    result_t r = allowlist_add(al, ACL_EMAIL, "alice@example.com");
    TEST("add returns OK");
    EXPECT_TRUE(r.status == OK);
    result_free(&r);

    TEST("enabled after first add");
    EXPECT_TRUE(allowlist_is_enabled(al, ACL_EMAIL));

    TEST("exact match allowed");
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "alice@example.com"));

    TEST("case-insensitive match allowed");
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "ALICE@EXAMPLE.COM"));

    TEST("unlisted address blocked");
    EXPECT_FALSE(allowlist_check(al, ACL_EMAIL, "eve@evil.com"));

    TEST("partial domain blocked");
    EXPECT_FALSE(allowlist_check(al, ACL_EMAIL, "alice@example.com.evil.org"));

    allowlist_destroy(al);
}

static void test_glob_patterns(void) {
    printf("\nGlob pattern matching\n");
    allowlist_t *al = allowlist_create();

    allowlist_add(al, ACL_EMAIL, "*@mycompany.com");

    TEST("wildcard: any user at domain allowed");
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "bob@mycompany.com"));

    TEST("wildcard: different domain blocked");
    EXPECT_FALSE(allowlist_check(al, ACL_EMAIL, "bob@othercompany.com"));

    TEST("wildcard: subdomain blocked (no partial match)");
    EXPECT_FALSE(allowlist_check(al, ACL_EMAIL, "bob@mail.mycompany.com"));

    allowlist_destroy(al);

    /* Command glob */
    allowlist_t *al2 = allowlist_create();
    allowlist_add(al2, ACL_COMMAND, "git *");

    TEST("command glob: git status allowed");
    EXPECT_TRUE(allowlist_check(al2, ACL_COMMAND, "git status"));

    TEST("command glob: git commit -m 'msg' allowed");
    EXPECT_TRUE(allowlist_check(al2, ACL_COMMAND, "git commit -m 'msg'"));

    TEST("command glob: rm not allowed");
    EXPECT_FALSE(allowlist_check(al2, ACL_COMMAND, "rm -rf /"));

    allowlist_destroy(al2);
}

static void test_rule_iteration(void) {
    printf("\nRule iteration (rule_count + rule_pattern)\n");
    allowlist_t *al = allowlist_create();

    TEST("count=0 before any adds");
    EXPECT_INT(allowlist_rule_count(al, ACL_EMAIL), 0);

    allowlist_add(al, ACL_EMAIL, "a@example.com");
    allowlist_add(al, ACL_EMAIL, "b@example.com");
    allowlist_add(al, ACL_EMAIL, "*@corp.com");

    TEST("count=3 after 3 adds");
    EXPECT_INT(allowlist_rule_count(al, ACL_EMAIL), 3);

    TEST("pattern 0 correct");
    EXPECT_STR(allowlist_rule_pattern(al, ACL_EMAIL, 0), "a@example.com");

    TEST("pattern 1 correct");
    EXPECT_STR(allowlist_rule_pattern(al, ACL_EMAIL, 1), "b@example.com");

    TEST("pattern 2 correct");
    EXPECT_STR(allowlist_rule_pattern(al, ACL_EMAIL, 2), "*@corp.com");

    TEST("out-of-bounds returns NULL");
    EXPECT_TRUE(allowlist_rule_pattern(al, ACL_EMAIL, 99) == NULL);

    TEST("negative index returns NULL");
    EXPECT_TRUE(allowlist_rule_pattern(al, ACL_EMAIL, -1) == NULL);

    TEST("other type still 0");
    EXPECT_INT(allowlist_rule_count(al, ACL_COMMAND), 0);

    allowlist_destroy(al);
}

static void test_remove(void) {
    printf("\nallowlist_remove\n");
    allowlist_t *al = allowlist_create();

    allowlist_add(al, ACL_EMAIL, "a@example.com");
    allowlist_add(al, ACL_EMAIL, "b@example.com");
    allowlist_add(al, ACL_EMAIL, "c@example.com");

    TEST("remove existing rule returns true");
    EXPECT_TRUE(allowlist_remove(al, ACL_EMAIL, "b@example.com"));

    TEST("count decremented after remove");
    EXPECT_INT(allowlist_rule_count(al, ACL_EMAIL), 2);

    TEST("removed address now blocked");
    EXPECT_FALSE(allowlist_check(al, ACL_EMAIL, "b@example.com"));

    TEST("remaining addresses still allowed");
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "a@example.com"));
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "c@example.com"));

    TEST("remove non-existent returns false");
    EXPECT_FALSE(allowlist_remove(al, ACL_EMAIL, "nobody@example.com"));

    TEST("remove last rule disables list");
    allowlist_remove(al, ACL_EMAIL, "a@example.com");
    allowlist_remove(al, ACL_EMAIL, "c@example.com");
    EXPECT_FALSE(allowlist_is_enabled(al, ACL_EMAIL));

    TEST("disabled after all rules removed: allow-all");
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "anyone@example.com"));

    allowlist_destroy(al);
}

static void test_set_enabled(void) {
    printf("\nallowlist_set_enabled\n");
    allowlist_t *al = allowlist_create();

    allowlist_add(al, ACL_EMAIL, "safe@example.com");

    TEST("initially enabled after add");
    EXPECT_TRUE(allowlist_is_enabled(al, ACL_EMAIL));

    allowlist_set_enabled(al, ACL_EMAIL, false);
    TEST("disabled after set_enabled(false)");
    EXPECT_FALSE(allowlist_is_enabled(al, ACL_EMAIL));

    TEST("allow-all when disabled");
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "anyone@attacker.com"));

    allowlist_set_enabled(al, ACL_EMAIL, true);
    TEST("re-enabled after set_enabled(true)");
    EXPECT_TRUE(allowlist_is_enabled(al, ACL_EMAIL));

    TEST("rule enforced after re-enable");
    EXPECT_FALSE(allowlist_check(al, ACL_EMAIL, "anyone@attacker.com"));

    allowlist_destroy(al);
}

static void test_load_from_file(void) {
    printf("\nallowlist_load (from temp file)\n");

    /* Write a temp config file */
    char tmpfile[] = "/tmp/dobby_test_allowlist_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) { printf("  SKIP (cannot create tmpfile)\n"); return; }

    const char *content =
        "# Dobby test allowlist\n"
        "\n"
        "[commands]\n"
        "allow = ls, cat, grep\n"
        "\n"
        "[email]\n"
        "allow = trusted@example.com, *@corp.org\n";

    write(fd, content, strlen(content));
    close(fd);

    allowlist_t *al = allowlist_create();
    result_t r = allowlist_load(al, tmpfile);

    TEST("load returns OK");
    EXPECT_TRUE(r.status == OK);
    result_free(&r);

    TEST("commands enabled after load");
    EXPECT_TRUE(allowlist_is_enabled(al, ACL_COMMAND));

    TEST("email enabled after load");
    EXPECT_TRUE(allowlist_is_enabled(al, ACL_EMAIL));

    TEST("path not enabled (section absent)");
    EXPECT_FALSE(allowlist_is_enabled(al, ACL_PATH));

    TEST("ls command allowed");
    EXPECT_TRUE(allowlist_check(al, ACL_COMMAND, "ls"));

    TEST("grep allowed");
    EXPECT_TRUE(allowlist_check(al, ACL_COMMAND, "grep"));

    TEST("rm blocked");
    EXPECT_FALSE(allowlist_check(al, ACL_COMMAND, "rm"));

    TEST("trusted email allowed");
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "trusted@example.com"));

    TEST("corp.org wildcard allowed");
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, "anyone@corp.org"));

    TEST("unknown email blocked");
    EXPECT_FALSE(allowlist_check(al, ACL_EMAIL, "spam@evil.net"));

    allowlist_destroy(al);
    unlink(tmpfile);
}

static void test_load_missing_file(void) {
    printf("\nallowlist_load — missing file (no-op)\n");
    allowlist_t *al = allowlist_create();
    result_t r = allowlist_load(al, "/tmp/does_not_exist_dobby_test.conf");

    TEST("load of missing file returns OK (soft error)");
    EXPECT_TRUE(r.status == OK);
    result_free(&r);

    TEST("all types still disabled");
    EXPECT_FALSE(allowlist_is_enabled(al, ACL_EMAIL));
    EXPECT_FALSE(allowlist_is_enabled(al, ACL_COMMAND));

    allowlist_destroy(al);
}

static void test_save_email(void) {
    printf("\nallowlist_save_email\n");

    char tmpfile[] = "/tmp/dobby_test_save_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) { printf("  SKIP (cannot create tmpfile)\n"); return; }
    close(fd);

    allowlist_t *al = allowlist_create();
    allowlist_add(al, ACL_EMAIL, "me@example.com");
    allowlist_add(al, ACL_EMAIL, "*@myorg.com");

    result_t r = allowlist_save_email(al, tmpfile);
    TEST("save returns OK");
    EXPECT_TRUE(r.status == OK);
    result_free(&r);

    /* Reload and verify */
    allowlist_t *al2 = allowlist_create();
    allowlist_load(al2, tmpfile);

    TEST("reloaded: email enabled");
    EXPECT_TRUE(allowlist_is_enabled(al2, ACL_EMAIL));

    TEST("reloaded: me@example.com allowed");
    EXPECT_TRUE(allowlist_check(al2, ACL_EMAIL, "me@example.com"));

    TEST("reloaded: wildcard *@myorg.com works");
    EXPECT_TRUE(allowlist_check(al2, ACL_EMAIL, "bob@myorg.com"));

    TEST("reloaded: unknown blocked");
    EXPECT_FALSE(allowlist_check(al2, ACL_EMAIL, "evil@hacker.com"));

    allowlist_destroy(al);
    allowlist_destroy(al2);
    unlink(tmpfile);
}

static void test_null_safety(void) {
    printf("\nNull-safety\n");
    allowlist_t *al = allowlist_create();

    TEST("check NULL value: returns true (allow)");
    /* NULL value with disabled list = allow */
    EXPECT_TRUE(allowlist_check(al, ACL_EMAIL, NULL));

    allowlist_add(al, ACL_EMAIL, "safe@example.com");

    TEST("rule_count NULL al: returns 0");
    EXPECT_INT(allowlist_rule_count(NULL, ACL_EMAIL), 0);

    TEST("rule_pattern NULL al: returns NULL");
    EXPECT_TRUE(allowlist_rule_pattern(NULL, ACL_EMAIL, 0) == NULL);

    TEST("remove NULL al: returns false");
    EXPECT_FALSE(allowlist_remove(NULL, ACL_EMAIL, "safe@example.com"));

    allowlist_destroy(al);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("━━━ Dobby Allowlist — Unit Tests ━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    test_create_disabled();
    test_add_and_check();
    test_glob_patterns();
    test_rule_iteration();
    test_remove();
    test_set_enabled();
    test_load_from_file();
    test_load_missing_file();
    test_save_email();
    test_null_safety();

    printf("\n━━━ Results: \033[32m%d passed\033[0m", g_pass);
    if (g_fail > 0) printf(", \033[31m%d FAILED\033[0m", g_fail);
    printf(" ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    return g_fail > 0 ? 1 : 0;
}
