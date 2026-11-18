#!/bin/bash
# Statusline wrapper: forwards Claude Code's statusline JSON (which carries
# rate_limits) to the buddy bridge, then hands the same JSON to the real
# statusline command unchanged. The POST is fire-and-forget so a dead
# bridge never slows or breaks the status line.
input=$(cat)
( printf '%s' "$input" | curl -s -m 0.5 -o /dev/null -X POST \
    -H 'Content-Type: application/json' --data-binary @- \
    http://127.0.0.1:47830/statusline >/dev/null 2>&1 & ) 2>/dev/null
printf '%s' "$input" | exec "${BUDDY_REAL_STATUSLINE:-$HOME/.local/bin/ccstatusline}"
