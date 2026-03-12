---
name: clawhub
description: Search and install agent skills from ClawHub, the public skill registry.
homepage: https://clawhub.ai
metadata: {"dobby":{"emoji":""}}
---

# ClawHub

Public skill registry for AI agents. Search by natural language (vector search).

## When to use

Use this skill when the user asks any of:
- "find a skill for "
- "search for skills"
- "install a skill"
- "what skills are available?"
- "update my skills"

## Search

```bash
npx --yes clawhub@latest search "web scraping" --limit 5
```

## Install

```bash
npx --yes clawhub@latest install <slug> --workdir ~/.dobby/
```

Replace `<slug>` with the skill name from search results. This places the skill into `~/.dobby/skills/`, where dobby loads workspace skills from. Always include `--workdir`.

## Update

```bash
npx --yes clawhub@latest update --all --workdir ~/.dobby/
```

## List installed

```bash
npx --yes clawhub@latest list --workdir ~/.dobby/workspace
```

## Notes

- Requires Node.js (`npx` comes with it).
- No API key needed for search and install.
- `--workdir ~/.dobby/workspace` is critical  without it, skills install to the current directory instead of the dobby workspace.
- After install, remind the user to start a new session to load the skill.
