#!/bin/bash
# Claude Code hook script — sends events to the Clawdmeter daemon.
# Usage: claude-hook.sh <event_type>
#
# Called by Claude Code hooks (configured in ~/.claude/settings.json).
# Receives hook context JSON on stdin.
#
# Event types:
#   stop               → chime when Claude finishes responding
#   permission_request  → interactive allow/deny on device
#   notification        → determines sub-type from stdin JSON
#   task_completed      → fanfare when a subagent finishes

DAEMON_URL="http://127.0.0.1:27182"
EVENT_TYPE="${1:-}"

# Read stdin (hook context from Claude Code)
STDIN_DATA="$(cat)"

case "$EVENT_TYPE" in
    permission_request)
        # Fire-and-forget: show permission dialog on device + play sound.
        # The device buttons send HID keystrokes to the Mac to approve/deny
        # the Claude Code permission prompt directly. No blocking needed.
        TOOL=$(echo "$STDIN_DATA" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
    print(d.get('tool_name', 'Unknown'))
except: print('Unknown')
" 2>/dev/null)

        CMD=$(echo "$STDIN_DATA" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
    inp = d.get('tool_input', {})
    if isinstance(inp, dict):
        print(inp.get('command', inp.get('file_path', inp.get('content', '')))[:120])
    else:
        print(str(inp)[:120])
except: print('')
" 2>/dev/null)

        # Send permission event (shows dialog + plays sound on device)
        PAYLOAD=$(python3 -c "
import json, sys
print(json.dumps({'event':'perm_dialog','tool':sys.argv[1],'cmd':sys.argv[2][:120]}))
" "$TOOL" "$CMD" 2>/dev/null)

        curl -s -m 5 -X POST "$DAEMON_URL/event" \
            -H "Content-Type: application/json" \
            -d "$PAYLOAD" \
            >/dev/null 2>&1 &
        ;;

    notification)
        # Detect notification sub-type from stdin JSON
        SUBTYPE=$(echo "$STDIN_DATA" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
    print(d.get('type', d.get('notification_type', '')))
except: print('')
" 2>/dev/null)

        case "$SUBTYPE" in
            permission_prompt)
                curl -s -m 5 -X POST "$DAEMON_URL/event" \
                    -H "Content-Type: application/json" \
                    -d '{"event":"permission_prompt"}' \
                    >/dev/null 2>&1 &
                ;;
            idle_prompt)
                curl -s -m 5 -X POST "$DAEMON_URL/event" \
                    -H "Content-Type: application/json" \
                    -d '{"event":"idle_prompt"}' \
                    >/dev/null 2>&1 &
                ;;
        esac
        ;;

    stop)
        # Fire-and-forget: Claude finished responding
        curl -s -m 5 -X POST "$DAEMON_URL/event" \
            -H "Content-Type: application/json" \
            -d '{"event":"stop"}' \
            >/dev/null 2>&1 &
        ;;

    task_completed)
        # Fire-and-forget: subagent task finished
        curl -s -m 5 -X POST "$DAEMON_URL/event" \
            -H "Content-Type: application/json" \
            -d '{"event":"task_completed"}' \
            >/dev/null 2>&1 &
        ;;
esac
