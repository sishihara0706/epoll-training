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
SHUTDOWN_TIMEOUT=5

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
    local watchdog_pid=""

    trap - EXIT
    set +e

    echo "=== Stopping server ==="

    if [ -n "$SERVER_PID" ]; then
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "Sending SIGTERM to PID $SERVER_PID"
            kill -TERM "$SERVER_PID"

            # Force stop if shutdown takes too long
            (
                sleep "$SHUTDOWN_TIMEOUT"
                if kill -0 "$SERVER_PID" 2>/dev/null; then
                    echo "Server shutdown timeout" >&2
                    kill -KILL "$SERVER_PID" 2>/dev/null
                fi
            ) &
            watchdog_pid=$!
        else
            echo "Server exited before cleanup"
            test_status=1
        fi

        # Collect the actual server exit status
        wait "$SERVER_PID"
        server_status=$?

        if [ -n "$watchdog_pid" ]; then
            kill "$watchdog_pid" 2>/dev/null || true
            wait "$watchdog_pid" 2>/dev/null || true
        fi

        if [ "$server_status" -ne 0 ]; then
            echo "Server exit status: $server_status"
            test_status=1
        fi
    fi

    echo "=== Server log ==="
    cat "$SERVER_LOG"
    rm -f "$SERVER_LOG"

    if [ "$test_status" -eq 0 ]; then
        echo "TEST PASSED"
    else
        echo "TEST FAILED"
    fi

    exit "$test_status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "=== Starting server ==="

"$SERVER_BIN" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!

echo "Server PID: $SERVER_PID"

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

