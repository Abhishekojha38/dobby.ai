/*
 * main.c — Dobby AI Agent entry point
 *
 * ARCHITECTURE
 *   Dobby always runs as a daemon with a message bus at its core.
 *   Channels (CLI, HTTP, email, …) connect to the bus independently.
 *   By default both the HTTP gateway and the CLI REPL start together.
 *
 * CHANNELS
 *   CLI   — interactive terminal REPL (disable with --no-cli)
 *   HTTP  — REST API + web UI on port 8080 (disable with --no-http)
 *   Email — (Phase 3, not yet implemented)
 *
 * SESSIONS
 *   Each unique (channel, user-id) pair gets its own isolated agent_t.
 *   cli:local         — the terminal user
 *   http:<client-ip>  — one session per browser/client IP
 *
 * USAGE
 *   dobby                       # HTTP gateway + CLI REPL (default)
 *   dobby --no-cli               # HTTP gateway only (headless)
 *   dobby --no-http              # CLI only (no gateway)
 *   dobby --port 9090            # custom port
 *   dobby --debug                # verbose logging
 *
 * CONFIGURATION
 *   dobby.conf  — all runtime settings
 *   .env       — secrets only (API_KEY)
 *
 *   Priority: shell env > .env > dobby.conf > built-in defaults
 */
#define _GNU_SOURCE
#include "../core/dobby.h"
#include "../core/templates.h"
#include "../agent/agent.h"
#include "../agent/tool_registry.h"
#include "../providers/provider.h"
#include "../providers/ollama/ollama.h"
#include "../providers/openai/openai.h"
#include "../providers/registry.h"
#include "../memory/md_memory.h"
#include "../history/history.h"
#include "../security/allowlist.h"
#include "../tools/shell/shell_tool.h"
#include "../tools/file_ops/file_tool.h"
#include "../tools/scheduler/scheduler.h"
#include "../tools/skills/skills.h"
#include "../tools/serial/serial_tool.h"
#include "../heartbeat/heartbeat.h"
#include "../bus/bus.h"
#include "../session/session.h"
#include "../channels/channel.h"
#include "../gateway/gateway.h"
#include "../channels/email/email_channel.h"
#include "../tools/email/email_tool.h"
#include "../providers/http_client.h"
#include "../ux/slash_commands.h"
#include "../ux/typing.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAS_READLINE
#  include <readline/readline.h>
#  include <readline/history.h>
#else
static char *readline(const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return strdup(buf);
}
static void add_history(const char *line) { (void)line; }
#endif

/* Global singletons */

static agent_t     *g_agent     = NULL;
static provider_t  *g_provider  = NULL;
static memory_t    *g_memory    = NULL;
static history_t   *g_history   = NULL;
static allowlist_t *g_allowlist = NULL;
static scheduler_t *g_scheduler = NULL;
static skills_t    *g_skills    = NULL;
static heartbeat_t *g_heartbeat = NULL;
static gateway_t         *g_gateway   = NULL;
static bus_t             *g_bus        = NULL;
static email_channel_t   *g_email       = NULL;
static session_manager_t *g_sessions   = NULL;
static int                g_memory_slot = -1;
static char               g_workspace[512] = ".";

/* Heartbeat: HEARTBEAT.md check context */
typedef struct {
    bus_t *bus;
    char   path[512];
} hb_tasks_ctx_t;

static hb_tasks_ctx_t g_hb_tasks_ctx;

/*
 * heartbeat_check_tasks — reads HEARTBEAT.md and injects it into the
 * agent bus as an agent_turn on the "heartbeat" channel if the file
 * contains active tasks (non-empty lines under "## Active Tasks").
 *
 * The agent processes it as a normal conversation turn and can use all
 * tools (send_email, file_write, memory_store, shell_exec, etc.) to
 * complete the tasks. When done it can update the file itself.
 */
static char *heartbeat_check_tasks(void *user_data) {
    hb_tasks_ctx_t *ctx = (hb_tasks_ctx_t *)user_data;
    if (!ctx || !ctx->bus) return NULL;

    FILE *f = fopen(ctx->path, "r");
    if (!f) return NULL;

    /* Quick scan: look for non-empty, non-comment lines under Active Tasks */
    bool in_active = false;
    bool has_tasks = false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "## Active Tasks", 15) == 0) { in_active = true;  continue; }
        if (strncmp(line, "## ",              3)  == 0) { in_active = false; continue; }
        if (!in_active) continue;
        /* Skip blank lines and HTML comments */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r') continue;
        if (strncmp(p, "<!--", 4) == 0) continue;
        has_tasks = true;
        break;
    }
    fclose(f);

    if (!has_tasks) return NULL;

    /* Inject instruction to process HEARTBEAT.md tasks */
    const char *msg =
        "Check HEARTBEAT.md in the workspace. "
        "Read the file, work through any tasks listed under '## Active Tasks', "
        "then move completed tasks to '## Completed' and update the file.";

    inbound_msg_t *imsg = inbound_msg_new(
        "heartbeat", "heartbeat", "heartbeat:tasks", msg, NULL);
    if (imsg) {
        if (!bus_publish_inbound(ctx->bus, imsg)) {
            LOG_WARN("Heartbeat: bus rejected HEARTBEAT.md task injection");
        }
    }

    LOG_DEBUG("Heartbeat: injected HEARTBEAT.md tasks into agent bus");
    return NULL; /* heartbeat system doesn't need a return message here */
}

/* Signals */

static volatile bool g_running      = true;
static volatile bool g_sigint_caught = false;

/* SIGTERM / SIGHUP → clean shutdown */
static void sig_term(int sig) { (void)sig; g_running = false; }

/* SIGINT (Ctrl-C):
 *   - While readline is waiting for input: cancel the line (stay alive).
 *   - While agent is thinking (STATE_THINKING): second Ctrl-C exits cleanly.
 * Ctrl-D (EOF) always exits. */
static volatile int g_sigint_count = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_sigint_caught = true;
    g_sigint_count++;
    /* Second Ctrl-C (or first while not in readline) forces exit */
    if (g_sigint_count >= 2) {
        g_running = false;
    }
#ifdef HAS_READLINE
#ifndef __APPLE__
    rl_done = 1;
#endif
#endif
}

/* Agent callbacks */

static void on_thinking(const char *status, void *ud) {
    (void)ud;
    status_set_detail(STATE_THINKING, status);
}

/* Helpers */

static char *load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf)  { fclose(f); return NULL; }
    buf[fread(buf, 1, (size_t)sz, f)] = '\0';
    fclose(f);
    return buf;
}

static const char *env_or_cfg(const char *env, config_t *cfg,
                               const char *sec, const char *key,
                               const char *def) {
    const char *v = getenv(env);
    return (v && *v) ? v : config_get(cfg, sec, key, def);
}

/* Refresh memory context in system prompt slot after each turn. */
/* Agent factory — creates a new agent_t for each new session.
 * Clones provider, settings, and system prompt from the template agent. */
typedef struct {
    agent_t *template_agent;
} agent_factory_ctx_t;

static agent_t *agent_factory(void *data) {
    agent_factory_ctx_t *ctx = (agent_factory_ctx_t *)data;
    agent_t *a = agent_create(g_provider);
    if (!a) return NULL;
    a->on_thinking    = NULL;   /* set only for cli:local session */
    a->max_iterations = ctx->template_agent->max_iterations;
    a->max_messages   = ctx->template_agent->max_messages;
    for (int i = 0; i < ctx->template_agent->system_part_count; i++)
        if (ctx->template_agent->system_parts[i])
            agent_add_system_part(a, ctx->template_agent->system_parts[i]);
    return a;
}

static void refresh_memory(agent_t *a) {
    if (g_memory_slot < 0 || g_memory_slot >= a->system_part_count) return;
    char *ctx = memory_get_context(g_memory);
    if (!ctx) return;
    free(a->system_parts[g_memory_slot]);
    a->system_parts[g_memory_slot] = ctx;

    if (a->message_count > 0 && !strcmp(a->messages[0].role, "system")) {
        free(a->messages[0].content);
        strbuf_t sb; strbuf_init(&sb, 4096);
        for (int i = 0; i < a->system_part_count; i++) {
            strbuf_append(&sb, a->system_parts[i]);
            strbuf_append(&sb, "\n\n");
        }
        a->messages[0].content = strdup(sb.data);
        strbuf_free(&sb);
    }
}

/* Cleanup */

static void cleanup(config_t *cfg) {
    status_shutdown();
    worker_stop();
    email_channel_destroy(g_email);       g_email    = NULL;
    session_manager_destroy(g_sessions);  g_sessions = NULL;
    bus_destroy(g_bus);                   g_bus      = NULL;
    gateway_destroy(g_gateway);
    heartbeat_destroy(g_heartbeat);
    skills_destroy(g_skills);
    scheduler_destroy(g_scheduler);
    memory_destroy(g_memory);
    agent_destroy(g_agent);
    if (g_provider) g_provider->destroy(g_provider);
    allowlist_destroy(g_allowlist);
    tool_registry_shutdown();
    http_cleanup();
    history_destroy(g_history);
    config_free(cfg);
    log_shutdown();
}

/* Usage */

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Dobby starts with both the HTTP gateway and CLI REPL by default.\n\n"
        "Options:\n"
        "      --no-cli            HTTP gateway only — no terminal REPL\n"
        "      --no-http           CLI REPL only — no HTTP gateway\n"
        "  -d, --daemon            Same as --no-cli (legacy)\n"
        "  -p, --port <port>       Gateway port (default: 8080)\n"
        "      --debug             Verbose request/tool logging\n"
        "  -l, --log-level <lvl>   debug | info | warn | error  (default: warn)\n"
        "  -L, --log-file <path>   Append logs to file\n"
        "  -c, --config <path>     Config file (default: dobby.conf)\n"
        "  -h, --help              Show this help\n\n"
        "Examples:\n"
        "  %s                      # HTTP + CLI (default)\n"
        "  %s --no-cli             # HTTP gateway only (headless daemon)\n"
        "  %s --no-http            # CLI only (no gateway)\n"
        "  %s --port 9090 --debug\n\n"
        "HTTP API:\n"
        "  curl -X POST http://HOST:8080/api/chat \\\n"
        "       -H 'Content-Type: application/json' \\\n"
        "       -d '{\"message\":\"Check disk usage\"}'\n"
        "  # Browser: http://HOST:8080/\n\n",
        argv0, argv0, argv0, argv0, argv0);
}

/* Argument parsing */

typedef struct {
    bool        no_cli;       /* --no-cli:  skip terminal REPL  */
    bool        no_http;      /* --no-http: skip HTTP gateway    */
    bool        debug_mode;
    int         port;
    const char *log_level;
    const char *log_file;
    const char *config;
} cli_args_t;

static bool parse_args(int argc, char *argv[], cli_args_t *out) {
    memset(out, 0, sizeof(*out));
    const char *debug_env = getenv("DEBUG");
    if (debug_env && (!strcmp(debug_env, "1") || !strcasecmp(debug_env, "true")))
        out->debug_mode = true;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-h") || !strcmp(a, "--help"))     { return false; }
        else if (!strcmp(a, "--no-cli"))                        { out->no_cli    = true; }
        else if (!strcmp(a, "--no-http"))                       { out->no_http   = true; }
        /* Legacy --daemon flag: HTTP-only (same as --no-cli) */
        else if (!strcmp(a, "-d") || !strcmp(a, "--daemon"))   { out->no_cli    = true; }
        else if (!strcmp(a, "--debug"))                         { out->debug_mode = true; }
        else if ((!strcmp(a, "-p") || !strcmp(a, "--port")) && i+1 < argc)
            { out->port = atoi(argv[++i]); }
        else if ((!strcmp(a, "-l") || !strcmp(a, "--log-level")) && i+1 < argc)
            { out->log_level = argv[++i]; }
        else if ((!strcmp(a, "-L") || !strcmp(a, "--log-file")) && i+1 < argc)
            { out->log_file = argv[++i]; }
        else if ((!strcmp(a, "-c") || !strcmp(a, "--config")) && i+1 < argc)
            { out->config = argv[++i]; }
        else { fprintf(stderr, "Unknown option: %s\n", a); return false; }
    }
    return true;
}

/* Resolve log level: CLI flag > LOG_LEVEL env > [logging] level in
 * dobby.conf > built-in default (warn).  cfg may be NULL (pre-config path). */
static log_level_t resolve_log_level(const char *flag, config_t *cfg) {
    const char *src = getenv("LOG_LEVEL");
    if (!src || !*src) src = flag;
    if ((!src || !*src) && cfg)
        src = config_get(cfg, "logging", "level", NULL);
    if (!src || !*src) return LOG_WARN;
    if (!strcmp(src, "debug")) return LOG_DEBUG;
    if (!strcmp(src, "info"))  return LOG_INFO;
    if (!strcmp(src, "error")) return LOG_ERROR;
    return LOG_WARN;
}

/* Common startup */

static config_t *boot(const cli_args_t *args) {
    /* .env file — loaded first so its vars feed both log init and dobby.conf.
     * Only secrets (API_KEY, proxy keys) should live here now.
     * Shell-exported vars always take precedence (setenv overwrite=0).
     * Path: ENV_FILE env var > default ".env". */
    {
        const char *env_path = getenv("ENV_FILE");
        if (!env_path || !*env_path) env_path = ".env";
        env_load(env_path);
    }

    /* Config — load before logging so [logging] section feeds log init */
    const char *cfg_path = args->config ? args->config : getenv("CONFIG");
    if (!cfg_path || !*cfg_path) cfg_path = "dobby.conf";
    config_t *cfg = config_load(cfg_path);

    /* Logging — priority: CLI flag > LOG_LEVEL env > [logging] level in
     * dobby.conf > built-in default (warn).  Log file follows same priority. */
    {
        const char *log_file = args->log_file
            ? args->log_file
            : (getenv("LOG_FILE")
                ? getenv("LOG_FILE")
                : config_get(cfg, "logging", "file", NULL));
        log_init(resolve_log_level(args->log_level, cfg), log_file);
    }

    /* Workspace — expand leading ~ to $HOME, then store in g_workspace */
    {
        const char *ws_raw = env_or_cfg("WORKSPACE", cfg, "paths", "workspace", "~/.dobby");
        const char *home   = getenv("HOME");
        if (ws_raw[0] == '~' && ws_raw[1] == '/' && home && *home)
            snprintf(g_workspace, sizeof(g_workspace), "%s/%s", home, ws_raw + 2);
        else if (ws_raw[0] == '~' && ws_raw[1] == '\0' && home && *home)
            snprintf(g_workspace, sizeof(g_workspace), "%s", home);
        else
            snprintf(g_workspace, sizeof(g_workspace), "%s", ws_raw);
    }
    /* Ensure workspace directory exists */
    {
        struct stat _ws_st;
        if (stat(g_workspace, &_ws_st) != 0) {
            if (mkdir(g_workspace, 0755) != 0)
                fprintf(stderr, "Warning: cannot create workspace '%s': %s\n",
                        g_workspace, strerror(errno));
        }
    }
    const char *workspace = g_workspace;

    /* Set TMUX_SOCKET so child processes (shell_exec, scripts) always
     * use the private socket — prevents agent tmux sessions from hijacking
     * the user's terminal or interfering with any existing tmux server.
     * Priority: TMUX_SOCKET env var > dobby.conf [tmux] socket > default */
    {
        const char *sock = env_or_cfg("TMUX_SOCKET", cfg,
                                      "tmux", "socket",
                                      "/tmp/dobby-tmux.sock");
        setenv("TMUX_SOCKET", sock, 0);
        LOG_DEBUG("TMUX_SOCKET=%s", sock);
    }

    /* Seed workspace from templates and bundled skills (never overwrites). */
    const char *tmpl_dir   = env_or_cfg("TEMPLATE_DIR", cfg, "paths", "templates", "templates");
    const char *skills_src = env_or_cfg("SKILLS_SRC",   cfg, "paths", "skills_src", "skills");
    templates_seed(tmpl_dir, skills_src, workspace);

    /* History — open before anything so startup events are logged */
    char hist_path[1024];
    snprintf(hist_path, sizeof(hist_path), "%s/memory/HISTORY.md", workspace);
    /* Ensure memory dir exists for history */
    char mem_dir[1024];
    snprintf(mem_dir, sizeof(mem_dir), "%s/memory", workspace);
    {   /* Create memory dir if missing */
        struct { char p[1024]; } tmp; snprintf(tmp.p, sizeof(tmp.p), "%s", mem_dir);
        /* Use mkdir directly — memory module also calls this, but history needs it first */
        mkdir(tmp.p, 0755);
    }
    g_history = history_create(hist_path);
    history_log(g_history, HISTORY_SYSTEM, "Agent Starting",
                      "version: " VERSION_STRING);

    /* Core subsystems */
    http_init();
    tool_registry_init();

    /* Allowlist */
    g_allowlist = allowlist_create();
    allowlist_load(g_allowlist,
        env_or_cfg("ALLOWLIST", cfg, "security", "allowlist", "allowlist.conf"));

    /* Provider */
    const char *provider_type = env_or_cfg("PROVIDER", cfg, "provider", "type", "ollama");
    const char *model   = env_or_cfg("MODEL",   cfg, "provider", "model",   NULL);
    /* api_url: explicit override for remote/self-hosted providers.
     * Read from API_URL env var or [provider] api_url in dobby.conf.
     * Kept separate from the Ollama URL so that setting url=http://localhost:11434
     * for Ollama does NOT bleed into remote provider calls. */
    const char *api_url = env_or_cfg("API_URL",  cfg, "provider", "api_url", NULL);
    const char *api_key = env_or_cfg("API_KEY",  cfg, "provider", "api_key", NULL);
    /* Ollama URL: its own key (ollama_url) with separate fallback chain.
     * Falls back to the legacy [provider] url key so existing configs still work,
     * then to the compiled-in default. */
    const char *ollama_url_cfg = config_get(cfg, "provider", "ollama_url", NULL);
    if (!ollama_url_cfg || !*ollama_url_cfg)
        ollama_url_cfg = config_get(cfg, "provider", "url", NULL);
    const char *ollama_url_env = getenv("OLLAMA_URL");
    const char *ollama_url = (ollama_url_env && *ollama_url_env) ? ollama_url_env
                           : (ollama_url_cfg && *ollama_url_cfg) ? ollama_url_cfg
                           : "http://localhost:11434";

    if (!model || !*model) {
        fprintf(stderr, "❌  No model set. Add 'model = ...' to dobby.conf or set MODEL.\n");
        history_log(g_history, HISTORY_ERROR, "Startup Failed", "No model configured");
        cleanup(cfg); exit(1);
    }

    if (!strcasecmp(provider_type, "ollama")) {
        g_provider = ollama_create(model, ollama_url);
        if (!g_provider) {
            fprintf(stderr,
                "❌  Cannot reach Ollama at %s\n"
                "    Start: ollama serve\n"
                "    Pull:  ollama pull %s\n", ollama_url, model);
            history_log(g_history, HISTORY_ERROR, "Startup Failed",
                              "Cannot connect to Ollama");
            cleanup(cfg); exit(1);
        }
    } else {
        /* Any OpenAI-compatible provider — registry handles URLs and headers */
        g_provider = openai_create(model, api_url, api_key, provider_type);
        if (!g_provider) {
            /* Try auto-detect from key/url if explicit name failed */
            const provider_spec_t *spec = registry_detect(api_key, api_url);
            if (spec) g_provider = openai_create(model, api_url, api_key, spec->name);
        }
        if (!g_provider) {
            fprintf(stderr,
                "❌  Cannot create provider '%s'.\n"
                "    Check API_KEY / dobby.conf [provider] api_key and url.\n",
                provider_type);
            history_log(g_history, HISTORY_ERROR, "Startup Failed",
                              "Cannot create provider");
            cleanup(cfg); exit(1);
        }
    }

    char hist_detail[1024];
    snprintf(hist_detail, sizeof(hist_detail),
             "provider: %s\nmodel: %s\nworkspace: %s",
             provider_type, model, workspace);
    history_log(g_history, HISTORY_SYSTEM, "Agent Initialized", hist_detail);

    /* Agent */
    g_agent = agent_create(g_provider);
    g_agent->on_thinking    = on_thinking;
    g_agent->max_iterations = config_get_int(cfg, "agent", "max_iterations", 10);
    g_agent->max_messages   = config_get_int(cfg, "agent", "max_messages",   20);

    /* Bootstrap .md files from workspace root */
    char soul_path[1024], agent_path[1024], tools_path[1024];
    snprintf(soul_path,  sizeof(soul_path),  "%s/SOUL.md",  workspace);
    snprintf(agent_path, sizeof(agent_path), "%s/AGENT.md", workspace);
    snprintf(tools_path, sizeof(tools_path), "%s/TOOLS.md", workspace);

    char *soul = load_file(soul_path);
    if (soul) { agent_add_system_part(g_agent, soul); free(soul); }
    char *inst = load_file(agent_path);
    if (inst) { agent_add_system_part(g_agent, inst); free(inst); }
    /* TOOLS.md documents non-obvious tool constraints; loaded after AGENT.md */
    char *tools_doc = load_file(tools_path);
    if (tools_doc) { agent_add_system_part(g_agent, tools_doc); free(tools_doc); }

    /* Tools */
    shell_tool_register(g_allowlist);
    file_tool_register(g_allowlist);

    /* Serial — device registry at workspace/platform_config/device.conf */
    {
        char serial_conf[1024];
        snprintf(serial_conf, sizeof(serial_conf),
                 "%s/platform_config/device.conf", workspace);
        serial_tool_register(serial_conf);
    }

    /* Memory — lives at workspace/memory/MEMORY.md */
    g_memory = memory_create(mem_dir);
    memory_register_tools(g_memory);
    char *mctx = memory_get_context(g_memory);
    g_memory_slot = g_agent->system_part_count;
    agent_add_system_part(g_agent, mctx);
    free(mctx);

    /* Scheduler — created after bus (see below) */

    /* Skills — workspace/skills/<name>/SKILL.md */
    g_skills = skills_create(workspace);
    char *sp = skills_get_prompt(g_skills);
    if (sp && *sp) agent_add_system_part(g_agent, sp);
    free(sp);

    /* Heartbeat */
    g_heartbeat = heartbeat_create(
        config_get_int(cfg, "heartbeat", "interval", 60));

    signal(SIGINT,  sig_handler);   /* Ctrl-C: see handler above */
    signal(SIGTERM, sig_term);
    signal(SIGHUP,  sig_term);

    /* ── Bus + sessions + channels ────────────────────────────────────── */
    g_bus = bus_create();

    /* Agent factory: each new session gets its own agent_t cloned from
     * g_agent so it shares the same provider, system prompt, and settings
     * but has an isolated conversation history. */
    static agent_factory_ctx_t g_factory_ctx;
    g_factory_ctx.template_agent = g_agent;
    g_sessions = session_manager_create(
        agent_factory,
        &g_factory_ctx,
        config_get_int(cfg, "agent", "session_ttl", 3600));

    /* Pre-create the CLI session and wire its on_thinking to the spinner */
    {
        session_t *cli_sess = session_get_or_create(g_sessions, "cli", "local");
        if (cli_sess) cli_sess->agent->on_thinking = on_thinking;
    }

    worker_start(g_bus, g_sessions);
    LOG_DEBUG("Bus, sessions, and worker threads ready");

    /* Scheduler — needs bus for agent-type tasks */
    g_scheduler = scheduler_create(g_bus, workspace);
    scheduler_register_tools(g_scheduler);

    /* Heartbeat — wire HEARTBEAT.md check and start */
    g_hb_tasks_ctx.bus = g_bus;
    snprintf(g_hb_tasks_ctx.path, sizeof(g_hb_tasks_ctx.path),
             "%s/HEARTBEAT.md", workspace);
    heartbeat_add_check(g_heartbeat, "tasks", heartbeat_check_tasks, &g_hb_tasks_ctx);
    heartbeat_start(g_heartbeat);

    /* ── Email channel ─────────────────────────────────────────── */
    g_email = email_channel_create(cfg, g_bus, g_allowlist);
    if (g_email && !email_channel_start(g_email))
        fprintf(stderr, "Warning: email channel failed to start\n");
    email_tool_register(g_email);  /* NULL-safe: shows config error if unconfigured */

    /* Inject Dobby's own email identity into the system prompt so the
     * agent knows its address and can use send_email correctly.
     * Added after email channel init so we have the real address. */
    {
        const char *my_addr = config_get(cfg, "email", "address", NULL);
        if (my_addr && *my_addr) {
            char identity[512];
            snprintf(identity, sizeof(identity),
                "## Email Identity\n"
                "Your email address is %s.\n"
                "To send email use the send_email tool with to, subject, and body.\n"
                "Never use shell commands to send email.",
                my_addr);
            agent_add_system_part(g_agent, identity);
        }
    }

    /* Status bar */
    status_init(model, args->no_cli);

    return cfg;
}

/* Daemon mode */

/* Shorten workspace path for display: /home/user/.dobby → ~/.dobby */
static const char *display_workspace(const char *ws) {
    const char *home = getenv("HOME");
    if (home && *home && strncmp(ws, home, strlen(home)) == 0) {
        static char buf[512];
        snprintf(buf, sizeof(buf), "~%s", ws + strlen(home));
        return buf;
    }
    return ws;
}

/*
 * run() — unified entry point.
 *
 * Always starts the bus and sessions.
 * Starts HTTP gateway unless --no-http.
 * Starts CLI REPL unless --no-cli.
 * If both are disabled, just waits for signals (useful for testing).
 */
static void run(const cli_args_t *args, config_t *cfg) {
    /* SIGHUP: if running headless, ignore it so the process survives terminal close.
     * If CLI is active, SIGHUP is already wired to sig_term in boot(). */
    if (args->no_cli) signal(SIGHUP, SIG_IGN);

    /* ── HTTP gateway ────────────────────────────────────────────────── */
    if (!args->no_http) {
        int port = args->port > 0 ? args->port
                 : atoi(env_or_cfg("PORT", cfg, "gateway", "port", "8080"));
        const char *html_path = env_or_cfg("CHAT_HTML", cfg,
                                            "gateway", "html_path", "assets/chat.html");
        g_gateway = gateway_create(g_bus, g_scheduler, port, html_path,
                                   args->debug_mode, g_email, g_allowlist);
        result_t r = gateway_start(g_gateway);
        if (r.status != OK) {
            fprintf(stderr, "❌  Gateway failed: %s\n", r.message);
            history_log(g_history, HISTORY_ERROR, "Gateway Failed", r.message);
            result_free(&r);
            cleanup(cfg); exit(1);
        }
        result_free(&r);
        char detail[256];
        snprintf(detail, sizeof(detail), "port: %d  debug: %s  html: %s",
                 port, args->debug_mode ? "on" : "off", html_path);
        history_log(g_history, HISTORY_SYSTEM, "HTTP Gateway Started", detail);
    }

    /* ── CLI REPL ────────────────────────────────────────────────────── */
    if (!args->no_cli) {
        const char *ws = display_workspace(g_workspace);
        printf("\n"
               "  \033[96m────────────────────────────────────────────────────\033[0m\n"
               "\033[1;35m  Dobby AI ⚡ \033[0m\n"
               "  Model    : \033[36m%s\033[0m\n"
               "  Version  : \033[32m" VERSION_STRING "\033[0m\n"
               "  Workspace: \033[33m%s\033[0m\n",
               g_provider->model, ws);
        if (!args->no_http) {
            int port = args->port > 0 ? args->port
                     : atoi(env_or_cfg("PORT", cfg, "gateway", "port", "8080"));
            printf("  HTTP     : \033[34mhttp://localhost:%d\033[0m\n", port);
        }
        printf("  Tip      : Ctrl-C×2 or Ctrl-D to quit, /help for commands\n"
               "  \033[96m────────────────────────────────────────────────────\033[0m\n"
               "\n");

        slash_ctx_t sctx = {
            .agent          = g_agent,
            .provider       = g_provider,
            .scheduler      = g_scheduler,
            .memory         = g_memory,
            .skills         = g_skills,
            .bus            = g_bus,
            .sessions       = g_sessions,
            .email          = g_email,
            .allowlist      = g_allowlist,
            .allowlist_path = env_or_cfg("ALLOWLIST", cfg, "security",
                                         "allowlist", "allowlist.conf"),
        };

        status_set(STATE_READY);

        while (g_running) {
            g_sigint_caught = false;
            status_readline_enter();   /* spinner must not write while readline owns terminal */
            char *input = readline("\033[2m>\033[0m ");
            status_readline_leave();

            if (!input && g_sigint_caught) { printf("\n"); continue; }
            if (!input) break;   /* Ctrl-D */

            g_sigint_count = 0;
            char *t = str_trim(input);
            if (!*t) { free(input); continue; }
            add_history(t);

            char slash_out[8192] = {0};
            if (slash_handle(t, &sctx, slash_out, sizeof(slash_out))) {
                if (!strcmp(slash_out, "__QUIT__")) { free(input); break; }
                printf("%s", slash_out);
                free(input); continue;
            }

            status_set(STATE_THINKING);
            response_pair_t rp;
            response_pair_init(&rp);
            char rp_meta[32];
            snprintf(rp_meta, sizeof(rp_meta), "rp=%llx",
                     (unsigned long long)(uintptr_t)&rp);
            inbound_msg_t *imsg = inbound_msg_new("cli", "local", "local", t, rp_meta);
            char *resp = NULL;
            if (imsg && bus_publish_inbound(g_bus, imsg)) {
                resp = response_pair_wait(&rp);
            } else {
                if (imsg) inbound_msg_free(imsg);
                fprintf(stderr, "Error: bus unavailable\n");
            }
            response_pair_destroy(&rp);
            status_set(STATE_READY);

            if (resp) {
                printf("\n\033[1;32m⚡ Dobby:\033[0m %s\n\n", resp);
                history_log(g_history, HISTORY_SYSTEM, "Conversation Turn",
                                  "status: completed");
                free(resp);
            }
            refresh_memory(g_agent);
            free(input);
        }

        printf("\n\033[2m⚡ Dobby signing off.\033[0m\n\n");

    } else {
        /* Headless — just wait for a signal */
        status_set(STATE_IDLE);
        while (g_running) pause();
        fprintf(stderr, "\nDobby shutting down…\n");
    }

    history_log(g_history, HISTORY_SYSTEM, "Agent Stopped", "clean shutdown");
    cleanup(cfg);
}

/* main */

int main(int argc, char *argv[]) {
    cli_args_t args;
    if (!parse_args(argc, argv, &args)) {
        print_usage(argv[0]);
        return 0;
    }

    config_t *cfg = boot(&args);
    run(&args, cfg);
    return 0;
}
