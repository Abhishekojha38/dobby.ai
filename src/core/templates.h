/*
 * templates.h — Workspace seeder
 *
 * Seeds workspace from bundled templates and skills on first run.
 * Existing files are never overwritten.
 *
 * Layout seeded:
 *   <template_dir>/SOUL.md            → <workspace>/SOUL.md
 *   <template_dir>/AGENT.md           → <workspace>/AGENT.md
 *   <template_dir>/TOOLS.md           → <workspace>/TOOLS.md
 *   <template_dir>/memory/MEMORY.md   → <workspace>/memory/MEMORY.md
 *   <skills_src>/<n>/SKILL.md         → <workspace>/skills/<n>/SKILL.md
 *   <skills_src>/<n>/scripts/*        → <workspace>/skills/<n>/scripts/*
 *
 * HISTORY.md is created by history.c on first startup.
 */
#ifndef TEMPLATES_H
#define TEMPLATES_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Seed workspace from templates and bundled skills.
 *
 * @template_dir: Path to bundled prompt templates (default: "templates").
 * @skills_src:   Path to bundled skills source dir (default: "skills").
 *                Pass NULL to skip skills seeding.
 * @workspace_dir: Workspace root (default: ".").
 */
void templates_seed(const char *template_dir,
                    const char *skills_src,
                    const char *workspace_dir);

#ifdef __cplusplus
}
#endif

#endif /* TEMPLATES_H */
