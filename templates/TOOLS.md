# Tool Notes

Tool signatures are auto-provided. Only non-obvious constraints are listed.

## file_read / file_write
- Paths are relative to workspace root.
- Read files before modifying.
- Binary files: use `shell_exec` + `xxd`.
- Max size: 10MB.

## memory_store / memory_search
- `memory_store(section,key,value)` → upsert in `MEMORY.md`.
- `memory_search(query)` → full-text search.
- Never edit `MEMORY.md` directly.
- Sections: User Information, Preferences, Project Context, Important Notes.

## send_whatsapp
Dobby tool only. Call via tool interface (never `shell_exec` or scripts).  
Parameter: `to` (phone with country code, e.g. "919876543210"), `text`.  
Purpose: send WhatsApp message via bridge.

## send_email
Dobby tool only. Call via tool interface; never via `shell_exec` or scripts.

## schedule_add
Task types:
- **agent_turn** (default): natural language task with full tool access.
- **shell**: raw shell command only.

Use `agent_turn` when tools are needed.  
Never bypass tools using shell scripts.

## spawn_subagent / subagent_status
Use to run tasks **in parallel** or in the background without blocking.

`spawn_subagent(name, task, context?)` — starts an isolated agent in its
own thread with its own context window and full tool access. Returns a
`subagent_id` immediately. Does NOT block.

`subagent_status(subagent_id?)` — poll for result.
- `status` values: `running` | `done` | `error` | `timeout`
- Omit `subagent_id` to list all sub-agents.

When to use:
- Long-running shell jobs or file processing
- Parallel research or data gathering
- Delegating a self-contained subtask (e.g. "draft an email, save to draft.md")

After spawning, continue other work and check back. Do not busy-wait in a loop.

## serial_execute
Config file: `<workspace>/platform_config/device.conf`.

Example:
`{"device":"m1700_0","commands":["uname -a"]}`

If device is missing, create it first with default config.

## tmux scripts
Location: `./skills/tmux/scripts/`

- `tmux-session.sh <n> [cmd]` — create sessions (detached, private socket)
- `wait-for-text.sh` — wait for pane output pattern
- `find-sessions.sh` — list sessions

Never run `tmux new-session` directly.

## HISTORY.md
Auto-written daemon audit log.

Do not read, write, or depend on it.  
Use `memory_search` to recall past events.