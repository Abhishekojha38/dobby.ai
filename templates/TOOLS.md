# Tool Notes

Tool signatures are auto-provided. Only non-obvious constraints are listed.

## file_read / file_write
- Paths are relative to workspace root.
- Read files before modifying them.
- Binary files: use `shell_exec` + `xxd`.
- Max size: 10MB.

## memory_store / memory_search
- `memory_store(section,key,value)` → upsert into `MEMORY.md`.
- `memory_search(query)` → full-text search.
- Never edit `MEMORY.md` directly.
- Sections: User Information, Preferences, Project Context, Important Notes.

## send_whatsapp
- Dobby tool only.
- Call via tool interface.
- Never use in `shell_exec` or scripts.
**send_whatsapp** — send a WhatsApp message via the bridge. Parameter: to (phone with country code e.g. "919876543210"), text. Never use shell for WhatsApp.

## send_email
- Dobby tool only.
- Call via tool interface.
- Never use in `shell_exec` or scripts.

## schedule_add
Task types:
- **agent_turn** (default): natural language task with full tool access.
- **shell**: raw shell command only.

Use `agent_turn` when tools are required.  
Never bypass tools with shell scripts.

## serial_execute
Config: `<workspace>/platform_config/device.conf`

Example:
`{"device":"m1700_0","commands":["uname -a"]}`

If device missing, create it with default config first.

## tmux scripts
Location: `./skills/tmux/scripts/`

- `tmux-session.sh <n> [cmd]` — create sessions only (detached + private socket)
- `wait-for-text.sh` — wait for pane output pattern
- `find-sessions.sh` — list sessions

Never run `tmux new-session` directly.

## HISTORY.md
Auto-written daemon audit log.

Do **not** read, write, or depend on it.  
Use `memory_search` to recall past events.