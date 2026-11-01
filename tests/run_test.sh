#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   bash tests/run_test.sh tests/load_test.py
#
# Environment:
#   SERVER_BIN=./build/server

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <test_script.py>" >&2
    exit 2
fi

TEST_SCRIPT="$1"
SERVER_BIN="${SERVER_BIN:-./build/server}"
HOST="127.0.0.1"
PORT=8080

if [ ! -f "$TEST_SCRIPT" ]; then
    echo "Test script not found: $TEST_SCRIPT" >&2
    exit 2
fi

if [ ! -x "$SERVER_BIN" ]; then
    echo "Server binary not executable: $SERVER_BIN" >&2
    exit 2
fi

SERVER_LOG="$(mktemp)"
SERVER_PID=""

cleanup() {
    local test_status=$?
    local server_status=0

    trap - EXIT
    set +e

    echo "=== Stopping server ==="

    if [ -n "$SERVER_PID" ]; then
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            kill -TERM "$SERVER_PID" 2>/dev/null
        else
            echo "Server exited unexpectedly"
            test_status=1
        fi

        wait "$SERVER_PID"
        server_status=$?

        if [ "$server_status" -ne 0 ]; then
            echo "Server exited with status: $server_status"
            test_status=1
        fi
    fi

    echo "=== Server log ==="
    cat "$SERVER_LOG"
    rm -f "$SERVER_LOG"

    if [ "$test_status" -ne 0 ]; then
        echo "TEST FAILED"
    else
        echo "TEST PASSED"
    fi

    exit "$test_status"
}

trap cleanup EXIT

echo "=== Starting server ==="

"$SERVER_BIN" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!

echo "Server PID: $SERVER_PID"

# Wait until the server accepts connections
ready=0

for i in {1..20}; do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "Server terminated during startup"
        exit 1
    fi

    if (true > /dev/tcp/$HOST/$PORT) 2>/dev/null; then
        ready=1
        break
    fi

    sleep 0.5
done

if [ "$ready" -ne 1 ]; then
    echo "Server readiness timeout"
    exit 1
fi

echo "Server is ready"

echo "=== Running $TEST_SCRIPT ==="

python3 "$TEST_SCRIPT"

