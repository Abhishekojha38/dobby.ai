/*
 * skills.h — Skills loader
 *
 * Skills live at:  workspace/skills/<name>/SKILL.md
 *
 * Each SKILL.md has a YAML frontmatter block:
 *
 *   ---
 *   name: Human-readable name
 *   description: One-line description shown in the skills summary
 *   always: true          # optional — inject full content into system prompt
 *   requires_bins: tio socat   # optional — space-separated CLI tools needed
 *   requires_env: MY_API_KEY   # optional — space-separated env vars needed
 *   ---
 *   ... skill content (Markdown) ...
 *
 * Progressive loading :
 *   always=true  → full content injected into system prompt at startup
 *   (default)    → only name/description shown; agent calls read_file
 *                  on workspace/skills/<name>/SKILL.md when needed
 *
 * available:
 *   A skill is available if all requires_bins are on PATH and all
 *   requires_env are set in the environment.
 *   Unavailable skills still appear in the summary (with available="false")
 *   so the agent knows they exist and can install dependencies.
 */
#ifndef SKILLS_H
#define SKILLS_H

#include "../tool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct skills skills_t;

/*
 * Create skills system scanning workspace/skills/ for subdirs with SKILL.md.
 * workspace_dir is the root workspace (default: current dir).
 */
skills_t *skills_create(const char *workspace_dir);

/*
 * Build the system-prompt fragment for skills:
 *   - Always skills: full SKILL.md content injected.
 *   - Available skills: XML summary block only.
 * Returns heap-allocated string (caller frees). Never returns NULL.
 */
char *skills_get_prompt(skills_t *skills);

/* Reload skills from disk (e.g. after /skills reload). */
result_t skills_reload(skills_t *skills);

/* Total skill count (always + available + unavailable). */
int skills_count(const skills_t *skills);

/* Get skill slug (directory name) by index. NULL if out of range. */
const char *skills_name(const skills_t *skills, int index);

/* Get skill one-line description by index. NULL if out of range. */
const char *skills_description(const skills_t *skills, int index);

/* Print a compact startup list to stdout. */
void skills_print_summary(const skills_t *skills);

/* Destroy skills system. */
void skills_destroy(skills_t *skills);

#ifdef __cplusplus
}
#endif

#endif /* SKILLS_H */
