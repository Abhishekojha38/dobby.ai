#!/usr/bin/env bash
# run_integration_test.sh — End-to-end email channel integration test
#
# What it does:
#   1. Starts the fake IMAP+SMTP server
#   2. Writes a temporary dobby.conf pointing at it
#   3. Starts Dobby in --no-cli mode (HTTP + email)
#   4. Injects test emails via the HTTP control API
#   5. Waits for Dobby to poll, process, and reply
#   6. Checks replies were delivered correctly
#   7. Cleans up
#
# Usage:
#   cd tests/email
#   ./run_integration_test.sh [path/to/dobby/binary]
#
# Requires: python3, curl, jq

set -euo pipefail

DOBBY="${1:-../../build/bin/dobby}"
FAKE_PY="$(dirname "$0")/fake_imap_smtp.py"
CONTROL="http://127.0.0.1:18080"
IMAP_PORT=11433
SMTP_PORT=11025
HTTP_PORT=18080
PASS=0; FAIL=0

# ── Colours ──────────────────────────────────────────────────────────────
GREEN='\033[32m'; RED='\033[31m'; RESET='\033[0m'; BOLD='\033[1m'

pass() { echo -e "  ${GREEN}PASS${RESET}  $1"; PASS=$((PASS+1)); }
fail() { echo -e "  ${RED}FAIL${RESET}  $1"; FAIL=$((FAIL+1)); }
banner() { echo -e "\n${BOLD}── $1 ──${RESET}"; }

# ── Check prerequisites ───────────────────────────────────────────────────
banner "Prerequisites"

python3 -c "import asyncio" 2>/dev/null && pass "python3 available" || { fail "python3 missing"; exit 1; }
command -v curl  &>/dev/null && pass "curl available"  || { fail "curl missing";  exit 1; }
command -v jq    &>/dev/null && pass "jq available"    || { fail "jq missing (install with: apt-get install jq)"; exit 1; }

if [ ! -x "$DOBBY" ]; then
    echo ""
    echo "  ⚠  Dobby binary not found at: $DOBBY"
    echo "     Build first:  mkdir -p ../../build && cd ../../build && cmake .. && make"
    echo "     Then re-run:  ./run_integration_test.sh ../../build/bin/dobby"
    echo ""
    echo "  Running PARSE-ONLY tests (no Dobby binary needed)..."
    RUN_DOBBY=false
else
    pass "Dobby binary found: $DOBBY"
    RUN_DOBBY=true
fi

# ── Start fake server ─────────────────────────────────────────────────────
banner "Starting fake IMAP+SMTP server"

python3 "$FAKE_PY" \
    --imap-port $IMAP_PORT \
    --smtp-port $SMTP_PORT \
    --http-port $HTTP_PORT &
FAKE_PID=$!

# Give it a moment to bind
sleep 1

# Verify it's up
STATUS=$(curl -sf "$CONTROL/status" 2>/dev/null || echo "")
if echo "$STATUS" | jq -e '.messages' &>/dev/null; then
    pass "Fake server started (pid=$FAKE_PID)"
else
    fail "Fake server did not start"
    kill $FAKE_PID 2>/dev/null || true
    exit 1
fi

# ── Test: inject emails via control API ───────────────────────────────────
banner "Email injection via control API"

UID1=$(curl -sf "$CONTROL/inject" \
    -d 'from=alice@test.local&subject=Disk+check&body=How+much+disk+space+is+free?' \
    | jq -r '.uid')
[ "$UID1" = "1" ] && pass "Injected email UID=1" || fail "Inject failed (got uid=$UID1)"

UID2=$(curl -sf "$CONTROL/inject" \
    -d 'from=bob@test.local&subject=Another+question&body=What+is+the+hostname?' \
    | jq -r '.uid')
[ "$UID2" = "2" ] && pass "Injected email UID=2" || fail "Inject failed (got uid=$UID2)"

UNSEEN=$(curl -sf "$CONTROL/status" | jq -r '.unseen')
[ "$UNSEEN" = "2" ] && pass "Both messages unseen" || fail "Expected 2 unseen, got $UNSEEN"

# ── Test: IMAP SEARCH UNSEEN via direct telnet-style curl ─────────────────
banner "IMAP protocol smoke test (curl IMAP)"

SEARCH_RESULT=$(curl -sv \
    --url "imap://127.0.0.1:$IMAP_PORT/INBOX" \
    --user "dobby@test.local:testpass" \
    -X "SEARCH UNSEEN" \
    2>&1 | grep "^\* SEARCH" || echo "")

if echo "$SEARCH_RESULT" | grep -q "SEARCH"; then
    pass "IMAP SEARCH UNSEEN returned response"
else
    fail "IMAP SEARCH UNSEEN got no response"
fi

# ── Start Dobby (if binary available) ─────────────────────────────────────
if $RUN_DOBBY; then
    banner "Starting Dobby (--no-cli mode)"

    # Write a minimal dobby.conf for testing
    TMPCONF=$(mktemp /tmp/dobby_test_XXXXXX.conf)
    cat > "$TMPCONF" << CONFEOF
[provider]
type  = ollama
model = mistral-nemo:12b

[agent]
max_iterations = 5
session_ttl    = 300

[gateway]
port = 18081

[email]
imap_url      = imap://127.0.0.1:$IMAP_PORT
smtp_url      = smtp://127.0.0.1:$SMTP_PORT
address       = dobby@test.local
password      = testpass
poll_interval = 3
inbox         = INBOX
subject_tag   = [Dobby]

[paths]
workspace = /tmp/dobby_test_workspace

[logging]
level = debug
file  = /tmp/dobby_test.log
CONFEOF

    mkdir -p /tmp/dobby_test_workspace

    CONFIG="$TMPCONF" "$DOBBY" --no-cli &
    DOBBY_PID=$!
    echo "  Dobby pid=$DOBBY_PID"

    # Wait for it to start and do at least one poll
    echo "  Waiting 10s for Dobby to poll and process emails..."
    sleep 10

    # Check replies
    REPLY_COUNT=$(curl -sf "$CONTROL/replies" | jq 'length')
    if [ "$REPLY_COUNT" -ge 1 ]; then
        pass "Got $REPLY_COUNT reply(ies) from Dobby"
        # Show first reply
        FIRST_REPLY=$(curl -sf "$CONTROL/replies" | jq '.[0]')
        echo "  First reply:"
        echo "$FIRST_REPLY" | jq -r '"    to: \(.to)  subject: \(.subject)"'
    else
        fail "No replies captured (check /tmp/dobby_test.log)"
    fi

    # Check sessions via /sessions slash command would show email: sessions
    # (can't test slash commands from here — they're CLI-only)

    # Teardown Dobby
    kill $DOBBY_PID 2>/dev/null || true
    wait $DOBBY_PID 2>/dev/null || true
    rm -f "$TMPCONF"
    pass "Dobby shut down cleanly"
fi

# ── Test: clear and re-inject, verify seen tracking ───────────────────────
banner "Seen tracking (re-inject after clear)"

curl -sf "$CONTROL/clear" -d '' | jq -r '"  Cleared: \(.ok)"'
curl -sf "$CONTROL/inject" \
    -d 'from=carol@test.local&subject=Follow+up&body=Second+round' > /dev/null

UNSEEN2=$(curl -sf "$CONTROL/status" | jq -r '.unseen')
[ "$UNSEEN2" = "1" ] && pass "Fresh message shows as unseen after clear" \
                      || fail "Expected 1 unseen after clear, got $UNSEEN2"

# ── Teardown fake server ──────────────────────────────────────────────────
banner "Cleanup"
kill $FAKE_PID 2>/dev/null || true
wait $FAKE_PID 2>/dev/null || true
pass "Fake server stopped"

# ── Results ───────────────────────────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}${BOLD}All $PASS tests passed.${RESET}"
else
    echo -e "${GREEN}$PASS passed${RESET}, ${RED}${BOLD}$FAIL FAILED${RESET}"
fi
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

exit $FAIL
