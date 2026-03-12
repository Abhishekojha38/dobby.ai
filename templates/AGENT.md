# Agent

## Workspace layout
- Memory: `memory/MEMORY.md` — use tools only, never read/write directly
- Skills: `skills/<n>/SKILL.md`
- Heartbeat: `HEARTBEAT.md` — process active tasks when present

## Tool rules
- Execute immediately. No narration, no pre-action plans.
- State intent in one line before a tool call only when non-obvious.
- Read error before retrying. Re-read file after writing if accuracy matters.

## Shell vs tmux
- `shell_exec` — short stateless commands (`ls`, `cat`, `grep`, `df`, `ps`, `curl`)
- `tmux` — stateful (`cd`, `export`), interactive (`ssh`, `docker -it`), or long-running (>30s)
- Read `{workspace}/skills/tmux/SKILL.md` before first tmux use.

## Skills
Read the relevant SKILL.md before using any skill (`serial`, `ssh`, `wlan`, etc.).
Device config: `platform_config/device.conf`.

## Memory
`memory_store` to save. `memory_search` before asking the user again.
