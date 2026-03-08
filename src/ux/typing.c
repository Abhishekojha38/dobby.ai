/*
 * typing.c — Animated spinner for the Dobby CLI
 *
 * THE TERMINAL CORRUPTION PROBLEM (and fix)
 * ─────────────────────────────────────────
 * readline() puts the terminal in raw mode and owns the cursor.
 * If the spinner thread writes to stderr while readline is active
 * (waiting for the next prompt), readline's internal display gets
 * out of sync with what the terminal shows. The result is doubled
 * characters, garbled input, and a broken prompt.
 *
 * Fix: a g_readline_active flag. The spinner thread checks it before
 * every write. The CLI sets it true before readline() and false
 * immediately after readline() returns. The spinner sees the flag,
 * skips its write, and the terminal stays clean.
 *
 * Sequence (correct):
 *   readline() called  → g_readline_active = true
 *   user types Enter   → g_readline_active = false
 *   status_set(THINKING) → spinner thread starts writing
 *   agent responds     → status_set(READY) → spinner joins
 *   readline() called  → g_readline_active = true   ← back to start
 */
#define _GNU_SOURCE
#include "typing.h"
#include "../core/types.h"
#include "../core/dobby.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *SPINNER[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"
};
#define SPINNER_COUNT 10

static const char *STATE_LABEL[] = {
    "Ready","Thinking","Executing Tool","Calling API","Idle"
};

/* ── Shared state ───────────────────────────────────────────────── */

static pthread_t         g_thread;
static volatile bool     g_active       = false;
static volatile bool     g_rl_active    = false;   /* readline owns terminal */
static char             *g_message      = NULL;
static pthread_mutex_t   g_mu           = PTHREAD_MUTEX_INITIALIZER;
static agent_state_t     g_state        = STATE_READY;
static bool              g_daemon_mode  = false;
static agent_state_t     g_last_daemon  = (agent_state_t)-1;

/* ── Spinner thread ─────────────────────────────────────────────── */

static void *spinner_fn(void *arg) {
    (void)arg;
    int frame = 0;

    while (g_active) {
        if (!g_rl_active) {
            /* Safe to write: readline is not active */
            pthread_mutex_lock(&g_mu);
            const char *msg = g_message ? g_message : "Thinking...";
            pthread_mutex_unlock(&g_mu);

            fprintf(stderr, "\r\033[K  %s %s",
                    SPINNER[frame], msg);
            fflush(stderr);
        }
        /* else: readline owns terminal — skip this frame silently */

        frame = (frame + 1) % SPINNER_COUNT;
        usleep(80000); /* 80 ms */
    }

    /* Clear spinner line only if we wrote anything */
    if (!g_rl_active) {
        fprintf(stderr, "\r\033[K");
        fflush(stderr);
    }
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────── */

void status_init(const char *model, bool daemon_mode) {
    (void)model;
    g_daemon_mode = daemon_mode;
    g_active      = false;
    g_rl_active   = false;
    g_state       = STATE_READY;
}

/* Called by the CLI REPL immediately before and after readline(). */
void status_readline_enter(void) { g_rl_active = true;  }
void status_readline_leave(void) { g_rl_active = false; }

void status_set(agent_state_t state) {
    status_set_detail(state, NULL);
}

void status_set_detail(agent_state_t state, const char *detail) {
    if (g_daemon_mode) {
        if (state != g_last_daemon) {
            g_last_daemon = state;
            fprintf(stderr, "  [%s]%s%s\n",
                    STATE_LABEL[state],
                    detail && detail[0] ? " " : "",
                    detail && detail[0] ? detail : "");
            fflush(stderr);
        }
        return;
    }

    pthread_mutex_lock(&g_mu);
    g_state = state;
    free(g_message);
    g_message = (detail && detail[0]) ? strdup(detail) : NULL;
    pthread_mutex_unlock(&g_mu);

    bool should_spin = (state == STATE_THINKING ||
                        state == STATE_TOOL      ||
                        state == STATE_API);

    if (should_spin && !g_active) {
        g_active = true;
        pthread_create(&g_thread, NULL, spinner_fn, NULL);
    } else if (!should_spin && g_active) {
        g_active = false;
        pthread_join(g_thread, NULL);
        /* After spinner stops, make sure line is clean */
        if (!g_rl_active) {
            fprintf(stderr, "\r\033[K");
            fflush(stderr);
        }
    }
}

void status_shutdown(void) {
    if (g_active) {
        g_active = false;
        pthread_join(g_thread, NULL);
    }
    pthread_mutex_lock(&g_mu);
    free(g_message);
    g_message = NULL;
    pthread_mutex_unlock(&g_mu);
    /* Final line clear */
    fprintf(stderr, "\r\033[K");
    fflush(stderr);
}
