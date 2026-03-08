/*
 * skills.c — Skills loader
 *
 * Directory layout:
 *   workspace/skills/<skill-name>/SKILL.md
 *
 * Frontmatter keys:
 *   name:          Human label (default: directory name)
 *   description:   One-liner for the XML summary
 *   always:        true → inject full content into system prompt at startup
 *   requires_bins: space-separated binaries that must be on PATH
 *   requires_env:  space-separated env vars that must be set
 *
 * Progressive loading:
 *   always=true + available → full content injected as "## Active Skills"
 *   available (not always)  → XML <skill available="true"> summary only;
 *                             agent reads the file with read_file on demand
 *   unavailable             → XML <skill available="false"> + missing deps
 */
#include "skills.h"
#include "../../core/log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SKILLS    64
#define MAX_REQUIRES   8
#define MAX_REQ_LEN   64

/* Skill record */

typedef struct {
    char  name[256];
    char  description[256];
    char  path[1040];
    char  slug[256];               /* directory name — unique key */
    bool  always;
    char  requires_bins[MAX_REQUIRES][MAX_REQ_LEN];
    int   n_bins;
    char  requires_env[MAX_REQUIRES][MAX_REQ_LEN];
    int   n_envs;
    bool  available;
    char *content;                /* body after frontmatter (heap) */
} skill_t;

struct skills {
    char    workspace[512];
    char    skills_dir[512];
    skill_t skills[MAX_SKILLS];
    int     count;
};

/* Helpers */

static bool cmd_on_path(const char *bin) {
    if (!bin || !*bin) return true;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", bin);
    return system(cmd) == 0;
}

static bool env_set(const char *var) {
    const char *v = getenv(var);
    return v && *v;
}

static int split_words(const char *src, char out[][MAX_REQ_LEN], int max) {
    int count = 0;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", src);
    char *tok = strtok(buf, " \t,");
    while (tok && count < max) {
        snprintf(out[count++], MAX_REQ_LEN, "%s", tok);
        tok = strtok(NULL, " \t,");
    }
    return count;
}

/* Frontmatter parser */

static bool parse_skill_file(const char *path, skill_t *sk) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* Defaults */
    snprintf(sk->name,        sizeof(sk->name),        "%s", sk->slug);
    snprintf(sk->description, sizeof(sk->description), "A custom skill");
    sk->always = false;
    sk->n_bins = 0;
    sk->n_envs = 0;

    strbuf_t body;
    strbuf_init(&body, 4096);

    char line[1024];
    int  dashes = 0;
    bool in_fm  = false;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len-1] == '\r') line[--len] = '\0';

        if (strcmp(line, "---") == 0) {
            dashes++;
            if (dashes == 1) { in_fm = true;  continue; }
            if (dashes == 2) { in_fm = false; continue; }
        }

        if (in_fm) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = '\0';
            const char *key = str_trim(line);
            const char *val = str_trim(colon + 1);

            if      (!strcmp(key, "name"))          snprintf(sk->name, sizeof(sk->name), "%s", val);
            else if (!strcmp(key, "description"))   snprintf(sk->description, sizeof(sk->description), "%s", val);
            else if (!strcmp(key, "always"))        sk->always = (!strcmp(val,"true")||!strcmp(val,"yes")||!strcmp(val,"1"));
            else if (!strcmp(key, "requires_bins")) sk->n_bins = split_words(val, sk->requires_bins, MAX_REQUIRES);
            else if (!strcmp(key, "requires_env"))  sk->n_envs = split_words(val, sk->requires_env,  MAX_REQUIRES);

        } else if (dashes >= 2) {
            strbuf_append(&body, line);
            strbuf_append(&body, "\n");
        }
    }
    fclose(f);

    /* No frontmatter → treat whole file as content */
    if (dashes < 2) {
        strbuf_free(&body);
        strbuf_init(&body, 4096);
        f = fopen(path, "r");
        if (f) { while (fgets(line, sizeof(line), f)) strbuf_append(&body, line); fclose(f); }
    }

    sk->content = strdup(body.data);
    strbuf_free(&body);

    /* Check requirements */
    sk->available = true;
    for (int i = 0; i < sk->n_bins && sk->available; i++)
        if (!cmd_on_path(sk->requires_bins[i])) sk->available = false;
    for (int i = 0; i < sk->n_envs && sk->available; i++)
        if (!env_set(sk->requires_env[i])) sk->available = false;

    return true;
}

/* Directory scan */

static void skills_free_entries(skills_t *s) {
    for (int i = 0; i < s->count; i++) { free(s->skills[i].content); s->skills[i].content = NULL; }
    s->count = 0;
}

static void skills_scan(skills_t *s) {
    skills_free_entries(s);

    DIR *d = opendir(s->skills_dir);
    if (!d) { LOG_DEBUG("Skills dir not found: %s", s->skills_dir); return; }

    struct dirent *ent;
    while ((ent = readdir(d)) && s->count < MAX_SKILLS) {
        if (ent->d_name[0] == '.') continue;

        char skill_dir[1024];
        snprintf(skill_dir, sizeof(skill_dir), "%s/%s", s->skills_dir, ent->d_name);

        struct stat st;
        if (stat(skill_dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        char skill_path[1040];
        snprintf(skill_path, sizeof(skill_path), "%s/SKILL.md", skill_dir);
        if (stat(skill_path, &st) != 0) continue;

        skill_t *sk = &s->skills[s->count];
        memset(sk, 0, sizeof(*sk));
        snprintf(sk->slug, sizeof(sk->slug), "%s", ent->d_name);
        snprintf(sk->path, sizeof(sk->path), "%s", skill_path);

        if (parse_skill_file(skill_path, sk)) {
            s->count++;
            LOG_DEBUG("Skill loaded: %s (always=%s available=%s)",
                      sk->name, sk->always?"true":"false", sk->available?"true":"false");
        }
    }
    closedir(d);
}

/* XML escape */

static void xml_append(strbuf_t *sb, const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '&': strbuf_append(sb, "&amp;");  break;
            case '<': strbuf_append(sb, "&lt;");   break;
            case '>': strbuf_append(sb, "&gt;");   break;
            case '"': strbuf_append(sb, "&quot;"); break;
            default:  { char c[2]={*s,'\0'}; strbuf_append(sb, c); }
        }
    }
}

/* Public API */

skills_t *skills_create(const char *workspace_dir) {
    skills_t *s = calloc(1, sizeof(skills_t));
    const char *ws = (workspace_dir && *workspace_dir) ? workspace_dir : ".";
    snprintf(s->workspace,  sizeof(s->workspace),  "%s", ws);
    snprintf(s->skills_dir, sizeof(s->skills_dir), "%s/skills", ws);

    struct stat st;
    if (stat(s->skills_dir, &st) != 0) mkdir(s->skills_dir, 0755);

    skills_scan(s);
    return s;
}

result_t skills_reload(skills_t *skills) {
    skills_scan(skills);
    return ok();
}

int skills_count(const skills_t *skills) {
    return skills ? skills->count : 0;
}

const char *skills_name(const skills_t *skills, int index) {
    if (!skills || index < 0 || index >= skills->count) return NULL;
    return skills->skills[index].slug;
}

const char *skills_description(const skills_t *skills, int index) {
    if (!skills || index < 0 || index >= skills->count) return NULL;
    return skills->skills[index].description;
}

/*
 * skills_get_prompt — Build system-prompt fragment.
 *
 *   ## Active Skills            ← always=true skills: full content
 *   ### Skill: <name>
 *   <content>
 *
 *   ## Skills                   ← XML summary for on-demand skills
 *   <skills>
 *     <skill available="true|false">
 *       <name>...</name>
 *       <description>...</description>
 *       <location>workspace/skills/<slug>/SKILL.md</location>
 *       [<requires>CLI: socat</requires>]
 *     </skill>
 *   </skills>
 */
char *skills_get_prompt(skills_t *skills) {
    strbuf_t sb;
    strbuf_init(&sb, 8192);

    /* Section 1 — always skills */
    bool has_always = false;
    for (int i = 0; i < skills->count; i++)
        if (skills->skills[i].always && skills->skills[i].available) { has_always = true; break; }

    if (has_always) {
        strbuf_append(&sb, "## Active Skills\n\n");
        for (int i = 0; i < skills->count; i++) {
            skill_t *sk = &skills->skills[i];
            if (!sk->always || !sk->available) continue;
            strbuf_appendf(&sb, "### Skill: %s\n\n%s\n\n---\n\n",
                                 sk->name, sk->content ? sk->content : "");
        }
    }

    /* Section 2 — XML summary for non-always skills */
    int summary_count = 0;
    for (int i = 0; i < skills->count; i++)
        if (!skills->skills[i].always) summary_count++;

    if (summary_count > 0) {
        strbuf_append(&sb,
            "## Skills\n\n"
            "The following skills extend your capabilities. "
            "To use a skill, read its SKILL.md with the read_file tool. "
            "Skills with available=\"false\" need dependencies installed first.\n\n"
            "<skills>\n");

        for (int i = 0; i < skills->count; i++) {
            skill_t *sk = &skills->skills[i];
            if (sk->always) continue;

            strbuf_appendf(&sb, "  <skill available=\"%s\">\n",
                                 sk->available ? "true" : "false");
            strbuf_append(&sb, "    <name>");
            xml_append(&sb, sk->name);
            strbuf_append(&sb, "</name>\n    <description>");
            xml_append(&sb, sk->description);
            strbuf_appendf(&sb, "</description>\n    <location>%s/skills/%s/SKILL.md</location>\n",
                                 skills->workspace, sk->slug);

            if (!sk->available) {
                strbuf_append(&sb, "    <requires>");
                bool first = true;
                for (int b = 0; b < sk->n_bins; b++) {
                    if (!cmd_on_path(sk->requires_bins[b])) {
                        if (!first) strbuf_append(&sb, ", ");
                        strbuf_append(&sb, "CLI: ");
                        xml_append(&sb, sk->requires_bins[b]);
                        first = false;
                    }
                }
                for (int e = 0; e < sk->n_envs; e++) {
                    if (!env_set(sk->requires_env[e])) {
                        if (!first) strbuf_append(&sb, ", ");
                        strbuf_append(&sb, "ENV: ");
                        xml_append(&sb, sk->requires_env[e]);
                        first = false;
                    }
                }
                strbuf_append(&sb, "</requires>\n");
            }

            strbuf_append(&sb, "  </skill>\n");
        }
        strbuf_append(&sb, "</skills>\n");
    }

    char *result = strdup(sb.data);
    strbuf_free(&sb);
    return result;
}

/*
 * skills_print_summary — Name-only startup list.
 *
 * Example:
 *   Skills   : memory*  shell_helper*  code_review  serial[needs:socat]
 *              (* = always loaded into context)
 */
void skills_print_summary(const skills_t *skills) {
    if (!skills || skills->count == 0) {
        printf("  Skills   : (none)\n");
        return;
    }
    printf("  Skills   : ");
    for (int i = 0; i < skills->count; i++) {
        const skill_t *sk = &skills->skills[i];
        if (i > 0) printf("  ");
        if (sk->always && sk->available) {
            /* magenta + asterisk = always-loaded */
            printf("\033[35m%s*\033[0m", sk->slug);
        } else if (sk->available) {
            /* green = available on demand */
            printf("\033[32m%s\033[0m", sk->slug);
        } else {
            /* dim + [needs:...] = unavailable */
            char miss[128] = "";
            for (int b = 0; b < sk->n_bins; b++) {
                if (!cmd_on_path(sk->requires_bins[b])) {
                    if (miss[0]) strncat(miss, ",", sizeof(miss)-strlen(miss)-1);
                    strncat(miss, sk->requires_bins[b], sizeof(miss)-strlen(miss)-1);
                }
            }
            for (int e = 0; e < sk->n_envs; e++) {
                if (!env_set(sk->requires_env[e])) {
                    if (miss[0]) strncat(miss, ",", sizeof(miss)-strlen(miss)-1);
                    strncat(miss, sk->requires_env[e], sizeof(miss)-strlen(miss)-1);
                }
            }
            printf("\033[2m%s[needs:%s]\033[0m", sk->slug, miss);
        }
    }
    printf("\n  \033[2m(* = always loaded into context)\033[0m\n");
}

void skills_destroy(skills_t *skills) {
    if (!skills) return;
    skills_free_entries(skills);
    free(skills);
}
