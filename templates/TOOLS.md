# Tool Notes

Tool signatures are provided automatically. This covers non-obvious constraints only.

## file_read / file_write
Relative paths resolve from workspace root. Binary files: use `shell_exec` with `xxd`.
Always `file_read` before `file_write` on existing files. 10MB size limit.

## memory_store / memory_search
`memory_store(section, key, value)` — upserts into MEMORY.md.
`memory_search(query)` — full-text search. Never write MEMORY.md directly.
Sections: User Information, Preferences, Project Context, Important Notes.

## send_email
`send_email` is a Dobby tool — NOT a shell command or binary.
Only call it via the function-calling mechanism. Never use it in shell_exec or shell scripts.

## schedule_add
Two task types:
- `agent_turn` (default) — `task` is a natural language instruction. Dobby runs a full agent
  turn with access to all tools. Use this for anything involving send_email, file ops, memory, etc.
- `shell` — `task` is a raw shell command. Use only for system tasks needing no Dobby tools.

Never write a shell script to work around tool access — just use type="agent_turn".

## serial_execute
Config at `<workspace>/platform_config/device.conf`. Use named device: `{"device":"m1700_0","commands":["uname -a"]}`.
If device missing from config, create it with default values first.

## tmux scripts (in ./skills/tmux/scripts/)
`tmux-session.sh <n> [cmd]` — only way to create sessions (enforces detached + private socket).
`wait-for-text.sh` — wait for pattern in pane. `find-sessions.sh` — list sessions.
Never run bare `tmux new-session`.

## HISTORY.md — do not touch
`HISTORY.md` is written automatically by the daemon as an internal audit log.
Never read it, write it, or treat it as a prerequisite for any operation.
If you need to recall past events, use `memory_search` — that queries MEMORY.md.
