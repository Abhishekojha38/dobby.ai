---
name: Cron
description: Schedule recurring tasks and manage the heartbeat task list
always: false
---

Two ways to run tasks automatically:

## 1. Scheduled Tasks (schedule_add)

Use `schedule_add` to repeat a task on a fixed interval.

**Important:** scheduled tasks cannot call tools directly as shell commands.
Always use `type="agent_turn"` (the default) for anything that needs tools.

```
schedule_add(
  name="daily hello",
  task="send an email to user@example.com with subject 'Daily Hello' and body 'Good morning!'",
  interval="every 24 hours"
)
```

The `task` field is a natural language instruction — agent runs a full agent turn to execute it, with access to all tools: send_email, file_write memory_store, shell_exec, etc.

Use `type="shell"` only for pure system commands that need no tools:
```
schedule_add(
  name="clean tmp",
  task="find /tmp -mtime +7 -delete",
  interval="every 24 hours",
  type="shell"
)
```

Manage tasks:
```
schedule_list()                                    # see all tasks
schedule_control(action="pause",  task_id=1)       # pause
schedule_control(action="resume", task_id=1)       # resume
schedule_control(action="delete", task_id=1)       # remove
```

## 2. Heartbeat Tasks (HEARTBEAT.md)

For one-off or loosely recurring tasks, edit `HEARTBEAT.md` in the workspace.
Dobby checks this file periodically and works through any active tasks it finds.

```
file_read("HEARTBEAT.md")
```

Add a task under `## Active Tasks`:
```
- Check disk usage and alert me if / is above 85%
- Create a summary of this week's work and save to memory
```

Once done, move it to `## Completed` or delete it.

**Use HEARTBEAT.md for:** one-time reminders, tasks to track manually, loose periodics.
**Use schedule_add for:** recurring tasks on a precise fixed timer.
