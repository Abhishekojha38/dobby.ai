/*
 * slash_commands.h — Slash command dispatcher for Dobby CLI
 *
 *   /help               — List all commands
 *   /status             — Agent, provider, model, message count
 *   /channels           — List active channels and sessions
 *   /sessions           — Active sessions with idle time and message count
 *   /bus                — Bus queue depths (inbound / outbound)
 *   /tools              — List registered tools with descriptions
 *   /skills             — List loaded skills
 *   /skills reload      — Re-scan skills/ directory from disk
 *   /tasks              — Active background scheduler tasks
 *   /tmux               — Active tmux sessions
 *   /new                — Start a fresh conversation (this session only)
 *   /model              — Show current model
 *   /usage              — Message count for this session
 *   /email status       — Email channel config and connection info
 *   /email allowlist    — Show allowed senders/recipients
 *   /email allow <addr> — Add address to email allowlist (persists to file)
 *   /email deny <addr>  — Remove address from email allowlist (persists)
 *   /email send <addr>  — Send a quick test email from Dobby
 *   /quit /exit         — Exit Dobby
 */
#ifndef SLASH_COMMANDS_H
#define SLASH_COMMANDS_H

#include "../core/dobby.h"

#ifdef __cplusplus
extern "C" {
#endif

struct agent;

typedef struct {
    struct agent       *agent;
    void               *provider;    /* provider_t*          */
    void               *scheduler;   /* scheduler_t*         */
    void               *memory;      /* memory_t*            */
    void               *skills;      /* skills_t*            */
    void               *bus;         /* bus_t*   — for /bus  */
    void               *sessions;    /* session_manager_t*   */
    void               *email;       /* email_channel_t*     */
    void               *allowlist;   /* allowlist_t*         */
    const char         *allowlist_path; /* path to allowlist.conf */
} slash_ctx_t;

bool slash_handle(const char *input, slash_ctx_t *ctx,
                  char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* SLASH_COMMANDS_H */
