# Tool Notes

Tool signatures are provided automatically. This covers non-obvious constraints only.

## file_read / file_write
Relative paths resolve from workspace root. Binary files: use `shell_exec` with `xxd`.
Always `file_read` before `file_write` on existing files. 10MB size limit.

## memory_store / memory_search
`memory_store(section, key, value)` — upserts into MEMORY.md.
`memory_search(query)` — full-text search. Never write MEMORY.md directly.
Sections: User Information, Preferences, Project Context, Important Notes.

## serial_execute
Config at `<workspace>/platform_config/device.conf`. Use named device: `{"device":"m1700_0","commands":["uname -a"]}`.
If device missing from config, create it with default values first.

## tmux scripts (in ./skills/tmux/scripts/)
`tmux-session.sh <n> [cmd]` — only way to create sessions (enforces detached + private socket).
`wait-for-text.sh` — wait for pattern in pane. `find-sessions.sh` — list sessions.
Never run bare `tmux new-session`.
