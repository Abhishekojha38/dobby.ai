/*
 * slash_commands.c — Slash command parser and dispatcher (Dobby)
 */
#define _GNU_SOURCE
#include "slash_commands.h"
#include "../core/log.h"
#include "../agent/agent.h"
#include "../agent/tool_registry.h"
#include "../providers/provider.h"
#include "../tools/skills/skills.h"
#include "../tools/scheduler/scheduler.h"
#include "../bus/bus.h"
#include "../session/session.h"
#include "../channels/channel.h"
#include "../channels/email/email_channel.h"
#include "../security/allowlist.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ── Helpers ───────────────────────────────────────────────────── */

static int buf_append(char *buf, size_t buf_size, const char *fmt, ...) {
    size_t used = strlen(buf);
    if (used >= buf_size - 1) return 0;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + used, buf_size - used, fmt, ap);
    va_end(ap);
    return written;
}

/* ── Command handlers ───────────────────────────────────────────── */

static void cmd_help(char *out, size_t sz) {
    snprintf(out, sz,
        "━━━ Dobby Commands ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "\033[1mAgent\033[0m\n"
        "  /status              — Provider, model, session message count\n"
        "  /usage               — Message count for this session\n"
        "  /new                 — Start a fresh conversation\n"
        "  /model               — Show current model\n"
        "\033[1mArchitecture\033[0m\n"
        "  /channels            — Registered channels (CLI, HTTP, Email…)\n"
        "  /sessions            — Active sessions: key, messages, idle time\n"
        "  /bus                 — Message bus queue depths\n"
        "\033[1mTools & Skills\033[0m\n"
        "  /tools               — All registered tools\n"
        "  /skills              — Loaded skills\n"
        "  /skills reload       — Re-scan skills/ directory from disk\n"
        "  /tasks               — Active background tasks\n"
        "  /tmux                — Active tmux sessions\n"
        "\033[1mEmail\033[0m\n"
        "  /email status        — Channel config and connection info\n"
        "  /email allowlist     — Show allowed senders/recipients\n"
        "  /email allow <addr>  — Add address to allowlist (persists)\n"
        "  /email deny  <addr>  — Remove address from allowlist (persists)\n"
        "  /email send  <addr>  — Send a quick test email to <addr>\n"
        "\033[1mOther\033[0m\n"
        "  /help                — Show this help\n"
        "  /quit                — Exit Dobby\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

static void cmd_status(char *out, size_t sz, slash_ctx_t *ctx) {
    provider_t *p = (provider_t *)ctx->provider;
    session_manager_t *sm = (session_manager_t *)ctx->sessions;
    int n_sess  = sm ? session_count(sm) : 0;
    int n_tools = tool_count();
    int n_skills = ctx->skills ? skills_count((skills_t *)ctx->skills) : 0;
    snprintf(out, sz,
        "━━━ Dobby Status ━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "  Version  : \033[32m%s\033[0m\n"
        "  Provider : \033[36m%s\033[0m\n"
        "  Model    : \033[36m%s\033[0m\n"
        "  Messages : %d  (this session)\n"
        "  Sessions : %d active\n"
        "  Channels : %d registered\n"
        "  Tools    : %d registered\n"
        "  Skills   : %d loaded\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n",
        VERSION_STRING,
        p ? p->name  : "none",
        p ? p->model : "none",
        ctx->agent ? agent_message_count(ctx->agent) : 0,
        n_sess,
        channel_count(),
        n_tools,
        n_skills);
}

static void cmd_channels(char *out, size_t sz) {
    out[0] = '\0';
    int n = channel_count();
    buf_append(out, sz,
        "━━━ Channels (%d registered) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n", n);
    if (n == 0) {
        buf_append(out, sz, "  No channels registered.\n");
    } else {
        /* channel_find by index not exposed — iterate known names */
        const char *known[] = { "cli", "http", "email", NULL };
        int shown = 0;
        for (int i = 0; known[i]; i++) {
            channel_t *ch = channel_find(known[i]);
            if (ch) {
                buf_append(out, sz, "  \033[36m%-12s\033[0m  active\n", ch->name);
                shown++;
            }
        }
        /* Show any unknown channels */
        if (shown < n)
            buf_append(out, sz, "  … and %d more\n", n - shown);
    }
    buf_append(out, sz,
        "  \033[2mSessions are isolated per channel:user pair.\033[0m\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

static void cmd_sessions(char *out, size_t sz, slash_ctx_t *ctx) {
    session_manager_t *sm = (session_manager_t *)ctx->sessions;
    out[0] = '\0';
    int n = sm ? session_count(sm) : 0;
    buf_append(out, sz,
        "━━━ Sessions (%d active) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n", n);
    if (!sm || n == 0) {
        buf_append(out, sz, "  No active sessions.\n");
    } else {
        char snap[4096] = {0};
        session_snapshot(sm, snap, sizeof(snap));
        buf_append(out, sz, "%s", snap);
    }
    buf_append(out, sz,
        "  \033[2mSessions expire after idle TTL. Use /new to reset this one.\033[0m\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

static void cmd_bus(char *out, size_t sz, slash_ctx_t *ctx) {
    bus_t *bus = (bus_t *)ctx->bus;
    if (!bus) { snprintf(out, sz, "Bus not available.\n"); return; }
    int inb  = bus_inbound_size(bus);
    int outb = bus_outbound_size(bus);
    snprintf(out, sz,
        "━━━ Message Bus ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "  Inbound queue  : %d message(s) pending\n"
        "  Outbound queue : %d message(s) pending\n"
        "  \033[2mInbound: channels → worker → agent\033[0m\n"
        "  \033[2mOutbound: agent → dispatcher → channels\033[0m\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n",
        inb, outb);
}

static void cmd_tools(char *out, size_t sz) {
    out[0] = '\0';
    int n = tool_count();
    buf_append(out, sz, "━━━ Tools (%d) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n", n);
    for (int i = 0; i < n; i++) {
        const tool_t *t = tool_get(i);
        if (!t) continue;
        buf_append(out, sz, "  \033[36m%-24s\033[0m %s\n", t->name, t->description);
    }
    buf_append(out, sz, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

static void cmd_skills(char *out, size_t sz, slash_ctx_t *ctx) {
    out[0] = '\0';
    skills_t *sk = (skills_t *)ctx->skills;
    int n = skills_count(sk);
    if (n == 0) {
        snprintf(out, sz, "No skills loaded. Add SKILL.md files to the skills/ directory.\n");
        return;
    }
    buf_append(out, sz, "━━━ Skills (%d) ━━━\n", n);
    for (int i = 0; i < n; i++) {
        const char *name = skills_name(sk, i);
        const char *desc = skills_description(sk, i);
        buf_append(out, sz, "  \033[35m%-24s\033[0m %s\n", name, desc ? desc : "");
    }
    buf_append(out, sz,
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "  \033[2mUse /skills reload to refresh from disk.\033[0m\n");
}

static void cmd_skills_reload(char *out, size_t sz, slash_ctx_t *ctx) {
    skills_t *sk = (skills_t *)ctx->skills;
    if (!sk) { snprintf(out, sz, "Skills system not available.\n"); return; }
    result_t r = skills_reload(sk);
    if (r.status == OK)
        snprintf(out, sz, "✓ Skills reloaded. %d skill(s) loaded.\n", skills_count(sk));
    else
        snprintf(out, sz, "Reload failed: %s\n", r.message ? r.message : "unknown error");
    result_free(&r);
}

static void cmd_tasks(char *out, size_t sz, slash_ctx_t *ctx) {
    scheduler_t *sched = (scheduler_t *)ctx->scheduler;
    out[0] = '\0';
    buf_append(out, sz, "━━━ Background Tasks ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    scheduler_task_snapshot(sched, out + strlen(out), sz - strlen(out));
    buf_append(out, sz,
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "  \033[2mUse schedule_add / schedule_control tools to manage.\033[0m\n");
}

static void cmd_tmux(char *out, size_t sz) {
    out[0] = '\0';
    buf_append(out, sz, "━━━ tmux Sessions ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    const char *sock = getenv("TMUX_SOCKET");
    char cmd[256];
    if (sock && *sock)
        snprintf(cmd, sizeof(cmd), "tmux -S '%s' ls 2>&1", sock);
    else
        snprintf(cmd, sizeof(cmd), "tmux ls 2>&1");
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        buf_append(out, sz, "  tmux not available.\n");
    } else {
        char line[256];
        int found = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "no server running") ||
                strstr(line, "no sessions") ||
                strstr(line, "error")) {
                buf_append(out, sz, "  No active tmux sessions.\n");
                found = -1; break;
            }
            buf_append(out, sz, "  \033[36m%s\033[0m", line);
            found++;
        }
        pclose(fp);
        if (found == 0) buf_append(out, sz, "  No active tmux sessions.\n");
    }
    buf_append(out, sz, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

/* ── Email commands ─────────────────────────────────────────────── */

static void cmd_email_status(char *out, size_t sz, slash_ctx_t *ctx) {
    email_channel_t *ec = (email_channel_t *)ctx->email;
    out[0] = '\0';
    buf_append(out, sz, "━━━ Email Channel ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    if (!ec) {
        buf_append(out, sz, "  Email channel not configured.\n"
            "  Add [email] section to dobby.conf to enable.\n");
    } else {
        char snap[1024] = {0};
        email_channel_status(ec, snap, sizeof(snap));
        buf_append(out, sz, "%s", snap);
    }
    buf_append(out, sz, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

static void cmd_email_allowlist(char *out, size_t sz, slash_ctx_t *ctx) {
    allowlist_t *al = (allowlist_t *)ctx->allowlist;
    out[0] = '\0';
    buf_append(out, sz, "━━━ Email Allowlist ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    if (!al || !allowlist_is_enabled(al, ACL_EMAIL)) {
        buf_append(out, sz,
            "  \033[33mAllowlist DISABLED\033[0m — all senders/recipients accepted.\n"
            "  Add [email] section to allowlist.conf to restrict.\n");
    } else {
        int n = allowlist_rule_count(al, ACL_EMAIL);
        buf_append(out, sz, "  \033[32mAllowlist ACTIVE\033[0m — %d rule(s):\n", n);
        for (int i = 0; i < n; i++) {
            buf_append(out, sz, "    \033[36m%2d.\033[0m  %s\n",
                       i + 1, allowlist_rule_pattern(al, ACL_EMAIL, i));
        }
        if (n == 0) buf_append(out, sz, "    (no rules — all email will be blocked)\n");
    }
    buf_append(out, sz,
        "  Use /email allow <addr>  to add a rule\n"
        "  Use /email deny  <addr>  to remove a rule\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

static void cmd_email_allow(char *out, size_t sz, slash_ctx_t *ctx,
                             const char *addr) {
    if (!addr || !*addr) {
        snprintf(out, sz, "Usage: /email allow <address-or-pattern>\n"
                          "  e.g. /email allow user@example.com\n"
                          "       /email allow *@mycompany.com\n");
        return;
    }
    allowlist_t *al = (allowlist_t *)ctx->allowlist;
    if (!al) { snprintf(out, sz, "Allowlist not available.\n"); return; }
    result_t r = allowlist_add(al, ACL_EMAIL, addr);
    if (r.status != OK) {
        snprintf(out, sz, "Error: %s\n", r.message ? r.message : "unknown");
        result_free(&r);
        return;
    }
    result_free(&r);
    /* Persist to file */
    if (ctx->allowlist_path) {
        result_t s = allowlist_save_email(al, ctx->allowlist_path);
        if (s.status == OK)
            snprintf(out, sz, "✓ Added \033[36m%s\033[0m to email allowlist (saved to %s).\n",
                     addr, ctx->allowlist_path);
        else
            snprintf(out, sz, "✓ Added \033[36m%s\033[0m to email allowlist (save failed: %s).\n",
                     addr, s.message ? s.message : "unknown");
        result_free(&s);
    } else {
        snprintf(out, sz, "✓ Added \033[36m%s\033[0m (runtime only — no config path set).\n", addr);
    }
}

static void cmd_email_deny(char *out, size_t sz, slash_ctx_t *ctx,
                            const char *addr) {
    if (!addr || !*addr) {
        snprintf(out, sz, "Usage: /email deny <address-or-pattern>\n");
        return;
    }
    allowlist_t *al = (allowlist_t *)ctx->allowlist;
    if (!al) { snprintf(out, sz, "Allowlist not available.\n"); return; }
    bool removed = allowlist_remove(al, ACL_EMAIL, addr);
    if (!removed) {
        snprintf(out, sz, "Address \033[36m%s\033[0m not found in email allowlist.\n", addr);
        return;
    }
    /* Persist */
    if (ctx->allowlist_path) {
        result_t s = allowlist_save_email(al, ctx->allowlist_path);
        if (s.status == OK)
            snprintf(out, sz, "✓ Removed \033[36m%s\033[0m from email allowlist (saved).\n", addr);
        else
            snprintf(out, sz, "✓ Removed \033[36m%s\033[0m (save failed: %s).\n",
                     addr, s.message ? s.message : "unknown");
        result_free(&s);
    } else {
        snprintf(out, sz, "✓ Removed \033[36m%s\033[0m (runtime only).\n", addr);
    }
}

static void cmd_email_send(char *out, size_t sz, slash_ctx_t *ctx,
                            const char *addr) {
    if (!addr || !*addr) {
        snprintf(out, sz, "Usage: /email send <address>\n"
                          "  Sends a quick test email from Dobby.\n");
        return;
    }
    email_channel_t *ec = (email_channel_t *)ctx->email;
    if (!ec) { snprintf(out, sz, "Email channel not configured.\n"); return; }
    if (!email_channel_is_allowed(ec, addr)) {
        snprintf(out, sz,
            "Blocked: \033[36m%s\033[0m is not in the email allowlist.\n"
            "Use /email allow %s first.\n", addr, addr);
        return;
    }
    bool ok = email_channel_send_direct(ec, addr,
        "[Dobby] Test email",
        "Hi,\n\nThis is a test email from Dobby AI Agent.\n\n— Dobby\n");
    if (ok)
        snprintf(out, sz, "✓ Test email sent to \033[36m%s\033[0m.\n", addr);
    else
        snprintf(out, sz, "✗ Failed to send to \033[36m%s\033[0m. Check SMTP config.\n", addr);
}

static void cmd_email(char *out, size_t sz, slash_ctx_t *ctx,
                       const char *subcmd) {
    const char *arg = str_trim((char *)subcmd);
    if (!*arg || !strcmp(arg, "status")) {
        cmd_email_status(out, sz, ctx); return;
    }
    if (!strcmp(arg, "allowlist") || !strcmp(arg, "list")) {
        cmd_email_allowlist(out, sz, ctx); return;
    }
    if (!strncmp(arg, "allow ", 6)) {
        cmd_email_allow(out, sz, ctx, str_trim((char *)(arg + 6))); return;
    }
    if (!strncmp(arg, "deny ", 5)) {
        cmd_email_deny(out, sz, ctx, str_trim((char *)(arg + 5))); return;
    }
    if (!strncmp(arg, "send ", 5)) {
        cmd_email_send(out, sz, ctx, str_trim((char *)(arg + 5))); return;
    }
    snprintf(out, sz,
        "Unknown email subcommand: %s\n"
        "  /email status       — channel info\n"
        "  /email allowlist    — show allowed addresses\n"
        "  /email allow <addr> — add to allowlist\n"
        "  /email deny  <addr> — remove from allowlist\n"
        "  /email send  <addr> — send test email\n", arg);
}

/* ── Dispatcher ─────────────────────────────────────────────────── */

bool slash_handle(const char *input, slash_ctx_t *ctx,
                  char *output, size_t output_size) {
    if (!input || input[0] != '/') return false;
    output[0] = '\0';

    if (!strcmp(input, "/help"))           { cmd_help(output, output_size); return true; }
    if (!strcmp(input, "/status"))         { cmd_status(output, output_size, ctx); return true; }
    if (!strcmp(input, "/channels"))       { cmd_channels(output, output_size); return true; }
    if (!strcmp(input, "/sessions"))       { cmd_sessions(output, output_size, ctx); return true; }
    if (!strcmp(input, "/bus"))            { cmd_bus(output, output_size, ctx); return true; }
    if (!strcmp(input, "/tools"))          { cmd_tools(output, output_size); return true; }
    if (!strcmp(input, "/skills reload"))  { cmd_skills_reload(output, output_size, ctx); return true; }
    if (!strcmp(input, "/skills"))         { cmd_skills(output, output_size, ctx); return true; }
    if (!strcmp(input, "/tasks"))          { cmd_tasks(output, output_size, ctx); return true; }
    if (!strcmp(input, "/tmux"))           { cmd_tmux(output, output_size); return true; }

    if (!strcmp(input, "/new")) {
        if (ctx->agent) agent_new_conversation(ctx->agent);
        snprintf(output, output_size, "✨ New conversation started.\n");
        return true;
    }

    if (!strncmp(input, "/model", 6)) {
        provider_t *p = (provider_t *)ctx->provider;
        const char *arg = str_trim((char *)(input + 6));
        if (!*arg)
            snprintf(output, output_size, "Model: %s (%s)\n",
                     p ? p->model : "none", p ? p->name : "none");
        else
            snprintf(output, output_size,
                "Set model = %s in dobby.conf and restart.\n", arg);
        return true;
    }

    if (!strcmp(input, "/usage")) {
        snprintf(output, output_size,
            "Messages in this session: %d\n",
            ctx->agent ? agent_message_count(ctx->agent) : 0);
        return true;
    }

    if (!strncmp(input, "/email", 6)) {
        cmd_email(output, output_size, ctx, input + 6);
        return true;
    }

    if (!strcmp(input, "/quit") || !strcmp(input, "/exit")) {
        snprintf(output, output_size, "__QUIT__");
        return true;
    }

    snprintf(output, output_size, "Unknown command: %s  (try /help)\n", input);
    return true;
}
