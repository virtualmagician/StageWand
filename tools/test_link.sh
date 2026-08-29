#!/usr/bin/env bash
# test_link.sh -- end-to-end test of StageWand's showlink against the mock
# StageWizard server.
#
# Starts tools/mock_stagewizard.py on NON-default ports (55100/55200, so it
# never collides with a real StageWizard instance), runs the already-built
# headless AmoledSim simulator against it with a synthetic tap on the GO
# button, then inspects the mock's log to confirm the round trip actually
# happened: the simulator's showlink subscribed (OSC feedback keepalive),
# fired /stagewizard/go from the tap, and got status back (either via OSC
# feedback push or the HTTP /status poll).
#
# NOTE: this script assumes AmoledSim already supports --link / --link-ports
# / --tap (added in parallel with this script). It is not run as part of
# writing it -- run it once the simulator side lands.
#
# Exit status: 0 if all three checks pass, 1 otherwise (or on setup failure).

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MOCK_PY="$REPO_ROOT/tools/mock_stagewizard.py"
SIM_BIN="$HOME/Library/Caches/AmoledSimBuild/debug/AmoledSim"
SNAPSHOT="/tmp/stagewand-link-test.png"
MOCK_LOG="$(mktemp -t stagewand-mock-log)"

OSC_PORT=55100
HTTP_PORT=55200
TAP_X=184
TAP_Y=240
FRAMES=400

MOCK_PID=""

PASS_COUNT=0
FAIL_COUNT=0

cleanup() {
    if [ -n "$MOCK_PID" ] && kill -0 "$MOCK_PID" 2>/dev/null; then
        kill "$MOCK_PID" 2>/dev/null
        wait "$MOCK_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

report() {
    # report <0|1> <label>
    if [ "$1" -eq 0 ]; then
        echo "PASS: $2"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "FAIL: $2"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

echo "== StageWand <-> StageWizard link test =="
echo "mock log: $MOCK_LOG"

if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: setup (python3 not found on PATH)"
    exit 1
fi

if [ ! -f "$MOCK_PY" ]; then
    echo "FAIL: setup (mock server not found: $MOCK_PY)"
    exit 1
fi

if [ ! -x "$SIM_BIN" ]; then
    echo "FAIL: setup (simulator binary not found or not executable: $SIM_BIN)"
    echo "      build it first (e.g. 'cd Simulator && swift build') before running this test."
    exit 1
fi

rm -f "$SNAPSHOT"

# --- start the mock -----------------------------------------------------
python3 "$MOCK_PY" \
    --osc-port "$OSC_PORT" \
    --http-port "$HTTP_PORT" \
    --osc-feedback \
    > "$MOCK_LOG" 2>&1 &
MOCK_PID=$!

# Wait for the mock to report it's listening (bounded poll, not a blind sleep).
READY=1
for _ in $(seq 1 50); do
    if ! kill -0 "$MOCK_PID" 2>/dev/null; then
        break
    fi
    if grep -q "MOCK listening" "$MOCK_LOG" 2>/dev/null; then
        READY=0
        break
    fi
    sleep 0.1
done

if [ "$READY" -ne 0 ]; then
    echo "FAIL: setup (mock server did not start; see $MOCK_LOG)"
    echo "--- mock log ---"
    cat "$MOCK_LOG" 2>/dev/null
    exit 1
fi

echo "mock started (pid $MOCK_PID) osc=:$OSC_PORT http=:$HTTP_PORT"

# --- run the simulator headless, pointed at the mock --------------------
echo "running: $SIM_BIN --snapshot $SNAPSHOT --frames $FRAMES --link 127.0.0.1 --link-ports $OSC_PORT,$HTTP_PORT --tap $TAP_X,$TAP_Y"
"$SIM_BIN" \
    --snapshot "$SNAPSHOT" \
    --frames "$FRAMES" \
    --link 127.0.0.1 \
    --link-ports "$OSC_PORT,$HTTP_PORT" \
    --tap "${TAP_X},${TAP_Y}"
SIM_STATUS=$?

if [ "$SIM_STATUS" -ne 0 ]; then
    echo "NOTE: simulator exited with status $SIM_STATUS (continuing to check the mock log anyway)"
fi

if [ -f "$SNAPSHOT" ]; then
    echo "NOTE: snapshot written to $SNAPSHOT"
else
    echo "NOTE: no snapshot found at $SNAPSHOT"
fi

# Give the mock a brief moment to flush the last log lines from the run.
sleep 0.2

# --- inspect the mock log -------------------------------------------------
echo "--- mock log ---"
cat "$MOCK_LOG"
echo "--- end mock log ---"

# 1. The simulator's showlink subscribed for OSC feedback (a ping or any
#    first packet triggers a FEEDBACK SUBSCRIBE line).
if grep -q "FEEDBACK SUBSCRIBE" "$MOCK_LOG"; then
    report 0 "client subscribed"
else
    report 1 "client subscribed"
fi

# 2. The tap fired /stagewizard/go.
if grep -q "/stagewizard/go" "$MOCK_LOG"; then
    report 0 "GO received"
else
    report 1 "GO received"
fi

# 3. The client got state back, either via the HTTP /status poll or an OSC
#    feedback push (either is a valid feedback path per showlink.h).
if grep -q "HTTP GET /status" "$MOCK_LOG" || grep -q "FEEDBACK PUSH" "$MOCK_LOG"; then
    report 0 "status served or feedback pushed"
else
    report 1 "status served or feedback pushed"
fi

echo "== $PASS_COUNT passed, $FAIL_COUNT failed =="

if [ "$FAIL_COUNT" -ne 0 ]; then
    exit 1
fi
exit 0
