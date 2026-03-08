#!/usr/bin/env bash
# run_tests.sh — Build and run all Dobby unit tests
#
# Usage:
#   cd /path/to/dobby
#   ./tests/run_tests.sh
#
#   Or with a pre-built binary:
#   ./tests/run_tests.sh --no-build

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"
PASS=0; FAIL=0

GREEN='\033[32m'; RED='\033[31m'; BOLD='\033[1m'; RESET='\033[0m'

banner() { echo -e "\n${BOLD}── $1 ──${RESET}"; }
pass()   { echo -e "  ${GREEN}PASS${RESET}  $1"; PASS=$((PASS+1)); }
fail()   { echo -e "  ${RED}FAIL${RESET}  $1"; FAIL=$((FAIL+1)); }

# ── Build ─────────────────────────────────────────────────────────────────
if [[ "${1:-}" != "--no-build" ]]; then
    banner "Building with tests enabled"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON "$ROOT" -G "Unix Makefiles" \
        2>&1 | tail -5
    make -j"$(nproc)" 2>&1 | grep -E "error:|warning:|Built target|Error" || true
    cd "$ROOT"
fi

# ── Run each test binary ───────────────────────────────────────────────────
banner "Unit Tests"

run_test() {
    local name="$1"
    local bin="$BUILD_DIR/$2"

    if [[ ! -x "$bin" ]]; then
        echo -e "  ${RED}SKIP${RESET}  $name (binary not found: $bin)"
        FAIL=$((FAIL+1))
        return
    fi

    if "$bin" > /tmp/dobby_test_output.txt 2>&1; then
        pass "$name"
        # Show summary line from output
        grep "Results:" /tmp/dobby_test_output.txt | sed 's/^/        /'
    else
        fail "$name"
        cat /tmp/dobby_test_output.txt | sed 's/^/        /'
    fi
}

run_test "Email parsing"   "bin/test_email_parsing"
run_test "Allowlist"       "bin/test_allowlist"
run_test "Config parser"   "bin/test_config"
run_test "History logger"  "bin/test_history"

# ── Integration tests (optional, needs python3 + jq) ──────────────────────
banner "Integration Tests"
if command -v python3 &>/dev/null && command -v jq &>/dev/null; then
    if [[ -x "$BUILD_DIR/bin/dobby" ]]; then
        echo "  Running email integration test..."
        if bash "$ROOT/tests/email/run_integration_test.sh" \
               "$BUILD_DIR/bin/dobby" > /tmp/dobby_integration.txt 2>&1; then
            pass "Email integration"
            grep "Results" /tmp/dobby_integration.txt | sed 's/^/        /'
        else
            fail "Email integration"
            tail -20 /tmp/dobby_integration.txt | sed 's/^/        /'
        fi
    else
        echo "  SKIP  Email integration (no dobby binary)"
    fi
else
    echo "  SKIP  Email integration (requires python3 + jq)"
fi

# ── Summary ───────────────────────────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
TOTAL=$((PASS+FAIL))
if [[ $FAIL -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}All $PASS/$TOTAL test suites passed.${RESET}"
else
    echo -e "${GREEN}$PASS passed${RESET}, ${RED}${BOLD}$FAIL FAILED${RESET} (of $TOTAL)"
fi
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
exit $FAIL
