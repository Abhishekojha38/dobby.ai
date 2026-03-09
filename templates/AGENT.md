# Agent Instructions

## Workspace
Workspace root is the configured directory.

- Memory: `{workspace}/memory/MEMORY.md`  
  Use `memory_store` / `memory_search` only. Never edit directly.
- Skills: `{workspace}/skills/<n>/SKILL.md`
- Heartbeat: `{workspace}/HEARTBEAT.md` — check periodically and process active tasks.

## Tool Guidelines
- State intent briefly before tool use.
- Do not predict results before receiving them.
- If a tool fails, read the error before retrying.
- Re-read files after writing when accuracy matters.
- Execute tools immediately — no narration or confirmation.

## Tools
Shell, file ops, scheduler, serial devices, memory, skills.

## shell_exec
Use for short stateless commands (e.g. `ls`, `cat`, `grep`, `df`, `ps`, `curl`).

## tmux
Use when:
- state must persist (`cd`, `export`, `source`)
- command is interactive (`ssh`, `docker -it`, `gdb`, `python`)
- runtime >30s (builds, `make`)

Read `skills/tmux/SKILL.md` first.

## Skills
Read the relevant skill before use (tmux, serial, ssh, wlan, network, linux).

Example:
`file_read("skills/serial/SKILL.md")`

Device config: `platform_config/device.conf`.

## Memory
Use `memory_store` to save facts.  
Use `memory_search` before asking the user again.

Never read/write `MEMORY.md` directly.

## Style
Concise. Act first, explain briefly after. No pre-action plans.