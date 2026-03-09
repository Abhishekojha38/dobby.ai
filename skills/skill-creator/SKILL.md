---
description: Create or update AgentSkills. Use when designing,
  structuring, or packaging skills with scripts, references, and assets.
name: skill-creator
---

# Skill Creator

Guidelines for designing effective AgentSkills.

## Skills Overview

Skills are modular packages that extend an agent with domain workflows,
tools, and reusable knowledge.\
They act as **task‑specific guides** that help the agent perform
specialized work reliably.

Skills typically provide:

1.  **Workflows** -- multi‑step procedures for common tasks\
2.  **Tool guidance** -- how to use APIs, formats, or CLIs\
3.  **Domain knowledge** -- schemas, rules, or business logic\
4.  **Resources** -- scripts, references, and templates

# Core Principles

## Keep Context Small

Skills share the context window with system prompts, history, and other
skills.

Only include information the agent **cannot easily infer**.

Prefer:

-   examples over long explanations
-   concise instructions
-   references for large documentation

## Control Degrees of Freedom

Choose instruction precision based on task reliability.

**High freedom** - general guidance - multiple valid approaches

**Medium freedom** - pseudocode or parameterized scripts

**Low freedom** - exact scripts or strict procedures

Use stricter instructions when errors are costly or workflows are
fragile.

# Skill Structure

A skill contains a required **SKILL.md** file and optional resources.

skill-name/ ├── SKILL.md ├── scripts/ ├── references/ └── assets/

## SKILL.md

### Frontmatter (required)

Contains only:

name\
description

The **description determines when the skill triggers**.

### Body

Instructions explaining how to use the skill and its resources.\
Loaded only after the skill is triggered.

# Resource Types

## scripts/

Executable automation (Python, Bash, etc.).

Use when:

-   a task requires deterministic execution
-   code would otherwise be repeatedly rewritten

Example:

scripts/rotate_pdf.py

Benefits:

-   token efficient
-   reliable execution
-   reusable across tasks

## references/

Documentation loaded only when needed.

Examples:

-   API specifications
-   database schemas
-   company policies
-   workflow documentation

Benefits:

-   keeps SKILL.md small
-   loads only when required

If large, include search hints or file references in SKILL.md.

## assets/

Files used in generated output but **not loaded into context**.

Examples:

-   templates
-   images or icons
-   fonts
-   project boilerplate

# Avoid Extra Files

Do not include unnecessary documentation such as:

-   README.md
-   installation guides
-   changelogs
-   internal notes

Skills should contain **only information required for the agent to
perform tasks**.

# Progressive Disclosure

Skills should minimize context usage through layered loading:

1.  **Metadata** -- always loaded (name + description)\
2.  **SKILL.md** -- loaded when the skill triggers\
3.  **Resources** -- loaded or executed only when needed

Guidelines:

-   keep SKILL.md concise (\<500 lines recommended)
-   move detailed documentation to references/
-   link reference files clearly from SKILL.md

# Skill Creation Workflow

1.  Understand how the skill will be used
2.  Identify reusable resources (scripts, references, assets)
3.  Initialize the skill
4.  Implement resources and write SKILL.md
5.  Package the skill
6.  Iterate based on real usage

# Skill Naming

Rules:

-   lowercase letters, digits, and hyphens only
-   convert titles to hyphen-case\
    Example: Plan Mode → plan-mode
-   keep names under 64 characters
-   prefer short, action‑focused names

Example:

gh-address-comments

The skill directory name must match the skill name.

# Step 1 --- Understand Usage

Gather concrete examples of user requests that should trigger the skill.

Example prompts:

-   "Rotate this PDF"
-   "Build a dashboard web app"
-   "Query today's login count"

Clarify expected workflows before implementation.

# Step 2 --- Plan Resources

Analyze each example and determine reusable components.

Examples:

PDF editing\
→ scripts/rotate_pdf.py

Frontend web apps\
→ assets/template/

Database queries\
→ references/schema.md

# Step 3 --- Initialize Skill

Create a new skill using:

scripts/init_skill.py `<skill-name>`{=html} --path `<output-dir>`{=html}

Optional options:

--resources scripts,references,assets\
--examples

This generates a template SKILL.md and optional resource directories.

# Step 4 --- Implement Skill

Add scripts, references, and assets as needed.

Guidelines:

-   write instructions in imperative form
-   keep SKILL.md concise
-   move large documentation into references/

Test any included scripts to ensure they run correctly.

# Step 5 --- Package Skill

Package and validate using:

scripts/package_skill.py `<skill-folder>`{=html}

Optional output directory:

scripts/package_skill.py `<skill-folder>`{=html} ./dist

The script validates:

-   frontmatter structure
-   skill naming rules
-   directory layout
-   resource references

If validation succeeds, a distributable file is created:

`<skill-name>`{=html}.skill

# Step 6 --- Iterate

Improve the skill using real usage feedback.

Typical loop:

1.  run the skill on real tasks
2.  observe friction or confusion
3.  update SKILL.md or resources
4.  test again
