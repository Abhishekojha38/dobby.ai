#!/usr/bin/env bash
# wait-for-text.sh — Poll a tmux pane until a pattern appears or timeout.
#
# Usage:
#   wait-for-text.sh -t <target> -p <pattern> [options]
#
# Options:
#   -t, --target    tmux target (session:window.pane)  REQUIRED
#   -p, --pattern   regex pattern to wait for          REQUIRED
#   -F, --fixed     treat pattern as fixed string (grep -F)
#   -S, --socket    tmux socket path (default: TMUX_SOCKET or system default)
#   -T, --timeout   seconds to wait (integer, default: 30)
#   -i, --interval  poll interval in seconds (default: 0.5)
#   -l, --lines     scrollback lines to search (integer, default: 1000)
#   -h, --help      show this help
#
# Exit codes:
#   0  pattern found
#   1  timed out
#   2  usage / config error
set -euo pipefail

usage() {
  sed -n '/^# Usage:/,/^[^#]/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
}

target=""
pattern=""
grep_flag="-E"
timeout=30
interval=0.5
lines=1000
socket_path="${TMUX_SOCKET:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -t|--target)   target="${2-}";      shift 2 ;;
    -p|--pattern)  pattern="${2-}";     shift 2 ;;
    -F|--fixed)    grep_flag="-F";      shift   ;;
    -S|--socket)   socket_path="${2-}"; shift 2 ;;
    -T|--timeout)  timeout="${2-}";     shift 2 ;;
    -i|--interval) interval="${2-}";    shift 2 ;;
    -l|--lines)    lines="${2-}";       shift 2 ;;
    -h|--help)     usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -z "$target"  ]] && { echo "error: --target is required" >&2; exit 2; }
[[ -z "$pattern" ]] && { echo "error: --pattern is required" >&2; exit 2; }

[[ "$timeout" =~ ^[0-9]+$ ]] || { echo "error: --timeout must be an integer" >&2; exit 2; }
[[ "$lines"   =~ ^[0-9]+$ ]] || { echo "error: --lines must be an integer"   >&2; exit 2; }

command -v tmux >/dev/null 2>&1 || { echo "error: tmux not found on PATH" >&2; exit 2; }

# Build base tmux command with optional socket
tmux_cmd=(tmux)
if [[ -n "$socket_path" ]]; then
  tmux_cmd+=(-S "$socket_path")
fi

deadline=$(( $(date +%s) + timeout ))

while true; do
  pane_text="$("${tmux_cmd[@]}" capture-pane -p -J -t "$target" -S "-${lines}" 2>/dev/null || true)"

  if printf '%s\n' "$pane_text" | grep -q $grep_flag -- "$pattern" 2>/dev/null; then
    exit 0
  fi

  now=$(date +%s)
  if (( now >= deadline )); then
    echo "Timed out after ${timeout}s waiting for: $pattern" >&2
    echo "--- last ${lines} lines from $target ---" >&2
    printf '%s\n' "$pane_text" >&2
    exit 1
  fi

  sleep "$interval"
done
