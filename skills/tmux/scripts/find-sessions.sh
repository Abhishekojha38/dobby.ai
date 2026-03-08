#!/usr/bin/env bash
# find-sessions.sh — List tmux sessions on the Dobby socket.
#
# Usage:
#   find-sessions.sh [-S socket-path] [-q pattern]
#
# Options:
#   -S, --socket   tmux socket path (default: TMUX_SOCKET or system default)
#   -q, --query    case-insensitive substring to filter session names
#   -h, --help     show this help
set -euo pipefail

socket_path="${TMUX_SOCKET:-}"
query=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -S|--socket) socket_path="${2-}"; shift 2 ;;
    -q|--query)  query="${2-}";       shift 2 ;;
    -h|--help)   sed -n '/^# Usage:/,/^[^#]/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

command -v tmux >/dev/null 2>&1 || { echo "error: tmux not found on PATH" >&2; exit 1; }

tmux_cmd=(tmux)
socket_label="default socket"
if [[ -n "$socket_path" ]]; then
  tmux_cmd+=(-S "$socket_path")
  socket_label="socket: $socket_path"
fi

if ! sessions="$("${tmux_cmd[@]}" list-sessions \
      -F '#{session_name}\t#{session_attached}\t#{session_created_string}' \
      2>/dev/null)"; then
  echo "No tmux server running on $socket_label" >&2
  exit 1
fi

if [[ -n "$query" ]]; then
  sessions="$(printf '%s\n' "$sessions" | grep -i -- "$query" || true)"
fi

if [[ -z "$sessions" ]]; then
  echo "No sessions found on $socket_label"
  exit 0
fi

echo "Sessions on $socket_label:"
printf '%s\n' "$sessions" | while IFS=$'\t' read -r name attached created; do
  label=$([[ "$attached" == "1" ]] && echo "attached" || echo "detached")
  printf '  %-30s  %-10s  %s\n' "$name" "$label" "$created"
done
