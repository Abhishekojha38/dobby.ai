/*
 * typing.h — Status display for the Dobby CLI
 *
 * No background thread. The spinner advances once per on_thinking() callback
 * call, which happens only while readline is NOT active (between Enter and
 * the printed reply). This eliminates all prompt/input corruption.
 */
#ifndef TYPING_H
#define TYPING_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATE_READY    = 0,   /* 🟢 idle, waiting for input     */
    STATE_THINKING = 1,   /* 🧠 LLM generating              */
    STATE_TOOL     = 2,   /* 🛠 tool call in progress       */
    STATE_API      = 3,   /* 📡 HTTP call in flight         */
    STATE_IDLE     = 4,   /* 💤 daemon with no activity     */
} agent_state_t;

/* Call once at startup. */
void status_init(const char *model, bool daemon_mode);

/* Update state. In CLI mode: draws/clears spinner. In daemon: logs to stderr. */
void status_set(agent_state_t state);

/* Call these immediately before / after every readline() call.
 * While readline is active the spinner will not write to stderr,
 * preventing terminal corruption (doubled characters, broken prompt). */
void status_readline_enter(void);
void status_readline_leave(void);

/* Same as status_set but with an optional sub-message. */
void status_set_detail(agent_state_t state, const char *detail);

/* Clear any on-screen spinner. Call before process exit. */
void status_shutdown(void);

/* Aliases used by the agent on_thinking callback */
#define typing_start(msg)  status_set_detail(STATE_THINKING, (msg))
#define typing_update(msg) status_set_detail(STATE_THINKING, (msg))
#define typing_stop()      status_set(STATE_READY)

#ifdef __cplusplus
}
#endif

#endif /* TYPING_H */
