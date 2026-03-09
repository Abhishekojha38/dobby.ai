/*
 * templates.c — Seed workspace from bundled templates and skills on first run
 *
 * Template layout (prompt files):
 *   <template_dir>/SOUL.md            → <workspace>/SOUL.md
 *   <template_dir>/AGENT.md           → <workspace>/AGENT.md
 *   <template_dir>/TOOLS.md           → <workspace>/TOOLS.md
 *   <template_dir>/memory/MEMORY.md   → <workspace>/memory/MEMORY.md
 *
 * Skills layout:
 *   <skills_src>/<skill>/SKILL.md          → <workspace>/skills/<skill>/SKILL.md
 *   <skills_src>/<skill>/scripts/*.sh      → <workspace>/skills/<skill>/scripts/*.sh
 *
 * Rules:
 *   - Never overwrites an existing file.
 *   - Creates directories as needed (0755).
 *   - .sh files in scripts/ are chmod +x after copy.
 *   - Safe to call on every startup.
 *
 * HISTORY.md is created by history.c — not seeded here.
 */
#include "templates.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Utilities ──────────────────────────────────────────────────── */

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        LOG_WARN("template: cannot create dir %s: %s", path, strerror(errno));
}

static void copy_if_missing(const char *src, const char *dst) {
    if (file_exists(dst)) return;
    FILE *in = fopen(src, "rb");
    if (!in) return;   /* src missing — silently skip */
    FILE *out = fopen(dst, "wb");
    if (!out) {
        LOG_WARN("template: cannot create %s: %s", dst, strerror(errno));
        fclose(in);
        return;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    LOG_DEBUG("template: seeded %s", dst);
}

/*
 * seed_dir — recursively copy src → dst, skipping existing files.
 * Depth-limited to 2 levels (skill/ + scripts/) — enough for our layout.
 */
static void seed_dir(const char *src, const char *dst, int depth) {
    if (depth > 2) return;

    ensure_dir(dst);

    DIR *d = opendir(src);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;

        char sp[1024], dp[1024];
        snprintf(sp, sizeof(sp), "%s/%s", src, ent->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, ent->d_name);

        struct stat st;
        if (stat(sp, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            seed_dir(sp, dp, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            copy_if_missing(sp, dp);
            /* Make shell scripts executable */
            size_t len = strlen(ent->d_name);
            if (len > 3 && strcmp(ent->d_name + len - 3, ".sh") == 0)
                chmod(dp, 0755);
        }
    }
    closedir(d);
}

/* ── Public API ─────────────────────────────────────────────────── */

void templates_seed(const char *template_dir,
                    const char *skills_src,
                    const char *workspace_dir) {
    if (!template_dir  || !*template_dir)  template_dir  = "templates";
    if (!workspace_dir || !*workspace_dir) workspace_dir = ".";

    /* ── Prompt files ────────────────────────────────────────────── */
    static const char *top[] = { "SOUL.md", "AGENT.md", "TOOLS.md", "HEARTBEAT.md", NULL };
    for (int i = 0; top[i]; i++) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/%s", template_dir,  top[i]);
        snprintf(dst, sizeof(dst), "%s/%s", workspace_dir, top[i]);
        copy_if_missing(src, dst);
    }

    /* ── memory/ ─────────────────────────────────────────────────── */
    char mem_dir[512];
    snprintf(mem_dir, sizeof(mem_dir), "%s/memory", workspace_dir);
    ensure_dir(mem_dir);
    char src_mem[512], dst_mem[512];
    snprintf(src_mem, sizeof(src_mem), "%s/memory/MEMORY.md", template_dir);
    snprintf(dst_mem, sizeof(dst_mem), "%s/memory/MEMORY.md", workspace_dir);
    copy_if_missing(src_mem, dst_mem);

    /* ── Skills ──────────────────────────────────────────────────── */
    if (!skills_src || !*skills_src) return;

    struct stat sst;
    if (stat(skills_src, &sst) != 0 || !S_ISDIR(sst.st_mode)) {
        LOG_DEBUG("template: skills source not found: %s", skills_src);
        return;
    }

    char skills_dst[512];
    snprintf(skills_dst, sizeof(skills_dst), "%s/skills", workspace_dir);
    ensure_dir(skills_dst);

    /* Each subdir of skills_src is one skill */
    DIR *d = opendir(skills_src);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char skill_src[1024], skill_dst[1024];
        snprintf(skill_src, sizeof(skill_src), "%s/%s", skills_src, ent->d_name);
        snprintf(skill_dst, sizeof(skill_dst), "%s/%s", skills_dst, ent->d_name);
        struct stat st;
        if (stat(skill_src, &st) == 0 && S_ISDIR(st.st_mode))
            seed_dir(skill_src, skill_dst, 1);
    }
    closedir(d);
    LOG_DEBUG("template: skills seeded from %s → %s", skills_src, skills_dst);
}
