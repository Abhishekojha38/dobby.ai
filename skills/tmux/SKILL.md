---
name: tmux
description: Open persistent terminal sessions and run interactive commands (docker, ssh, builds, REPLs) using tmux.
always: true
requires_bins: tmux
---

# tmux Skill

Use this skill whenever you need a persistent terminal — interactive commands,
docker containers, SSH sessions, long builds, or anything that needs state to
survive across multiple commands.

**Do not use `shell_exec` for interactive commands.** Use tmux sessions instead.

---

## Creating sessions — ALWAYS use tmux-session.sh

**Never run `tmux new-session` directly.** Always use the helper script:

```bash
./skills/tmux/scripts/tmux-session.sh <session-name> [command]
```

This script enforces the private socket and the detached flag automatically.
Direct `tmux new-session` calls risk hijacking the user's terminal.

```bash
# Plain bash session
./skills/tmux/scripts/tmux-session.sh mywork

# Start with a specific command
./skills/tmux/scripts/tmux-session.sh docker_work "docker run -it --rm debian:12 bash"
./skills/tmux/scripts/tmux-session.sh remote "ssh user@192.168.1.100"
```

The script prints the attach command — always show it to the user:
```
Session 'mywork' created on socket: /tmp/dobby-tmux.sock
To attach and observe: tmux -S "/tmp/dobby-tmux.sock" attach -t "mywork"
Detach without killing: Ctrl+b then d
```

---

## All other tmux commands — always use the socket

For everything after session creation, use `tmux -S "$SOCKET"` directly:

```bash
SOCKET="${TMUX_SOCKET:-/tmp/dobby-tmux.sock}"

# Send a command
tmux -S "$SOCKET" send-keys -t "$SESSION" "your command here" Enter

# Wait for completion
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p '\$\s*$' -T 30

# Read output
tmux -S "$SOCKET" capture-pane -p -J -t "$SESSION" -S -200

# Kill session when done
tmux -S "$SOCKET" kill-session -t "$SESSION"
```

`wait-for-text.sh` and `find-sessions.sh` automatically use `TMUX_SOCKET`
from the environment — no `-S` flag needed for those scripts.

---

## Sending input

```bash
SOCKET="${TMUX_SOCKET:-/tmp/dobby-tmux.sock}"

# Literal send — safe for commands with quotes or special characters
tmux -S "$SOCKET" send-keys -t "$SESSION" -l -- "your command"
tmux -S "$SOCKET" send-keys -t "$SESSION" Enter

# Simple form (for plain commands)
tmux -S "$SOCKET" send-keys -t "$SESSION" "ls -la" Enter

# Interrupt
tmux -S "$SOCKET" send-keys -t "$SESSION" C-c

# EOF / exit REPL
tmux -S "$SOCKET" send-keys -t "$SESSION" C-d
```

---

## Waiting for completion

Always wait before reading output or sending the next command.

```bash
# bash/sh prompt
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p '\$\s*$' -T 30

# root prompt
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p '#\s*$' -T 30

# docker container prompt
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p 'root@' -T 30

# specific completion string (fixed)
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -F -p "Build complete" -T 300

# python REPL
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p '>>>' -T 10

# Options: -T timeout(s)  -i interval(s)  -l scrollback-lines  -F fixed-string
```

---

## Reading output

```bash
SOCKET="${TMUX_SOCKET:-/tmp/dobby-tmux.sock}"

# Last ~50 visible lines
tmux -S "$SOCKET" capture-pane -p -J -t "$SESSION"

# Full scrollback (last N lines)
tmux -S "$SOCKET" capture-pane -p -J -t "$SESSION" -S -500
```

---

## Common patterns

### Docker container

```bash
./skills/tmux/scripts/tmux-session.sh docker_work "docker run -it --rm debian:12 bash"
SOCKET="${TMUX_SOCKET:-/tmp/dobby-tmux.sock}"
SESSION="docker_work"

./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p 'root@' -T 30

tmux -S "$SOCKET" send-keys -t "$SESSION" "apt-get install -y git" Enter
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p 'root@' -T 120

tmux -S "$SOCKET" capture-pane -p -J -t "$SESSION" -S -200
```

### SSH session

```bash
./skills/tmux/scripts/tmux-session.sh remote "ssh user@192.168.1.100"
SOCKET="${TMUX_SOCKET:-/tmp/dobby-tmux.sock}"
SESSION="remote"

./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p '\$\s*$' -T 30

tmux -S "$SOCKET" send-keys -t "$SESSION" "uname -a" Enter
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p '\$\s*$' -T 10
tmux -S "$SOCKET" capture-pane -p -J -t "$SESSION" -S -100
```

### Long build

```bash
./skills/tmux/scripts/tmux-session.sh build
SOCKET="${TMUX_SOCKET:-/tmp/dobby-tmux.sock}"
SESSION="build"

tmux -S "$SOCKET" send-keys -t "$SESSION" "cd /src && make -j$(nproc)" Enter
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p '\$\s*$' -T 3600

tmux -S "$SOCKET" capture-pane -p -J -t "$SESSION" -S -100
```

### Python REPL

```bash
./skills/tmux/scripts/tmux-session.sh python_repl
SOCKET="${TMUX_SOCKET:-/tmp/dobby-tmux.sock}"
SESSION="python_repl"

# PYTHON_BASIC_REPL=1 prevents readline magic that breaks send-keys
tmux -S "$SOCKET" send-keys -t "$SESSION" "PYTHON_BASIC_REPL=1 python3 -q" Enter
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p '>>>' -T 10

tmux -S "$SOCKET" send-keys -t "$SESSION" "print('hello')" Enter
./skills/tmux/scripts/wait-for-text.sh -t "$SESSION" -p '>>>' -T 10
tmux -S "$SOCKET" capture-pane -p -J -t "$SESSION" -S -50
```

---

## Listing sessions

```bash
./skills/tmux/scripts/find-sessions.sh
./skills/tmux/scripts/find-sessions.sh -q "docker"
```

---

## Cleanup

```bash
SOCKET="${TMUX_SOCKET:-/tmp/dobby-tmux.sock}"

# Kill one session
tmux -S "$SOCKET" kill-session -t "$SESSION"

# Kill all Dobby sessions
tmux -S "$SOCKET" kill-server
```
