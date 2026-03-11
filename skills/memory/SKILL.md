---
name: Memory
description: Persist and recall information across sessions using MEMORY.md and HISTORY.md
always: true
---

You have persistent memory that survives across sessions.

## Files

| File | Purpose |
|------|---------|
| `memory/MEMORY.md` | Structured long-term memory — facts, preferences, project context |
| `memory/HISTORY.md` | Append-only event log — grep-searchable record of past actions |

**memory** folder is located at `{workspace}/memory/`

## Search Past Events

Choose the search method based on file size:

- Small `{workspace}/memory/HISTORY.md`: use `read_file`, then search in-memory
- Large or long-lived `{workspace}/memory/HISTORY.md`: use the `exec` tool for targeted search

Examples:
- **Linux/macOS:** `grep -i "keyword" {workspace}/memory/HISTORY.md`
- **Windows:** `findstr /i "keyword" {workspace}/memory/HISTORY.md`
- **Cross-platform Python:** `python -c "from pathlib import Path; text = Path('workspace/memory/HISTORY.md').read_text(encoding='utf-8'); print('\n'.join([l for l in text.splitlines() if 'keyword' in l.lower()][-20:]))"`

Prefer targeted command-line search for large history files.

## When to Update MEMORY.md

Write important facts immediately using `edit_file` or `write_file`:
- User preferences ("I prefer dark mode")
- Project context ("The API uses OAuth2")
- Relationships ("Alice is the project lead")

## Reading memory

Read the full memory file before answering questions about the user or their projects:

```
read_file("{workspace}/memory/MEMORY.md")
```

Search for a specific topic:

```
memory_search(query="project name")
```

Grep history for past events:

```
shell_exec(command="grep -i 'keyword' {workspace}/memory/HISTORY.md")
```

## Writing memory

Use `memory_store` to save facts. Choose the right section:

| Section | Use for |
|---------|---------|
| `User Information` | Name, email, role, location, contact details |
| `Preferences` | Communication style, tools, formatting preferences |
| `Project Context` | Active projects, goals, tech stack, decisions |
| `Important Notes` | Anything else worth remembering |


Write important facts immediately using `memory_store`:
- User preferences ("I prefer dark mode")
- User information ("My name is Abhishek Ojha")

To update a fact, call `memory_store` with the same key — it will replace the existing entry.

## Rules

- **Always read before answering** questions about past conversations, the user, or their projects.
- **Write proactively** when the user shares something they'd want remembered: names, preferences, decisions, project details.
- **Prefer updating** an existing key over creating a near-duplicate.
- **Never modify HISTORY.md** — it is append-only. The system writes to it automatically.
- If the user asks "do you remember…" — check memory first, then say if you don't have it.