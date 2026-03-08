#!/usr/bin/env bash
# tmux-session.sh — Create a detached Dobby tmux session on the private socket.
#
# This is the ONLY script the agent should use to create tmux sessions.
# It enforces:
#   - Private socket (TMUX_SOCKET) — never the user's default tmux
#   - Always detached (-d) — never hijacks the current terminal
#   - Safe defaults (220x50 window size)
#
# Usage:
#   tmux-session.sh <session-name> [command]
#
#   session-name   Required. Name for the new session (e.g. "build", "docker_work")
#   command        Optional. Command to run in the session (default: bash)
#
# Examples:
#   tmux-session.sh build
#   tmux-session.sh docker_work "docker run -it --rm debian:12 bash"
#   tmux-session.sh kernel_build "bash"
#
# After creation, attach from your terminal with:
#   tmux -S "$TMUX_SOCKET" attach -t <session-name>
#   Detach: Ctrl+b then d
set -euo pipefail

SOCKET="${TMUX_SOCKET:-/tmp/dobby-tmux.sock}"
SESSION="${1:-}"
COMMAND="${2:-bash}"

if [[ -z "$SESSION" ]]; then
    echo "Usage: tmux-session.sh <session-name> [command]" >&2
    exit 1
fi

command -v tmux >/dev/null 2>&1 || { echo "error: tmux not found on PATH" >&2; exit 1; }

# Kill existing session with same name if present (clean slate)
tmux -S "$SOCKET" kill-session -t "$SESSION" 2>/dev/null || true

# Create session — always detached, always on private socket
tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 220 -y 50

# If a non-bash command was requested, send it now
if [[ "$COMMAND" != "bash" ]]; then
    tmux -S "$SOCKET" send-keys -t "$SESSION" "$COMMAND" Enter
fi

echo "Session '$SESSION' created on socket: $SOCKET"
echo "To attach and observe: tmux -S \"$SOCKET\" attach -t \"$SESSION\""
echo "Detach without killing: Ctrl+b then d"
