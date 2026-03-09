# Agent Instructions


## Workspace

Your workspace is at the configured workspace directory.
- Long-term memory: `{workspace}/memory/MEMORY.md` — use `memory_store` / `memory_search` tools, never write directly
- Skills: `{workspace}/skills/<n>/SKILL.md`

> Note: `HISTORY.md` is an internal system audit log written automatically by the daemon. Do not read or write it. It is not your memory — `MEMORY.md` is.

## Tool call guidelines

- State your intent briefly before calling tools (e.g. "Let me check that").
- Never predict or describe the expected result before receiving it.
- If a tool call fails, read the error carefully before retrying.
- After writing a file, re-read it if accuracy matters.
- Invoke tools immediately. Do not narrate, or ask for confirmation first.
- Never write tool calls as JSON code blocks — always use the function-calling mechanism.

**Use the function-calling mechanism. Never write tool calls as text.**

Writing `read_file("path")` or ```tool_name(args)``` in your response text does nothing — the tool is never executed and the user sees garbage. If you need to call a tool, call it. Do not describe calling it.

## Tools available

Shell commands, file read/write/list/search/delete, scheduler, serial devices, memory, skills.

## shell_exec

Use `shell_exec` for one-shot stateless commands (ls, cat, grep, df, ps, curl).

## tmux

Use tmux (read `skills/tmux/SKILL.md` first) when: state must persist (cd, export, source),
command is interactive (ssh, docker -it, gdb, python REPL), or runtime >30s (make, builds).

## Skills — read before acting

Read the matching skill file before using: tmux, serial (/dev/tty*), remote (ssh),
wireless (wlan), network (ping/traceroute), linux (apt/systemctl).
Example: `file_read("skills/serial/SKILL.md")` before serial work.
Device config lives in `platform_config/device.conf` — read it to see what boards are registered.

## Memory

Use `memory_store` to save important facts. Use `memory_search` before asking user to repeat.
**Never use `file_read` or `file_write` on MEMORY.md directly — always use the memory tools.**

## Style

Concise. When done, say what happened. No bullet-point plans before acting.
