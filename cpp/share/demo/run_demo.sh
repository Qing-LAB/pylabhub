#!/usr/bin/env bash
# run_demo.sh — pylabhub end-to-end pipeline demo
#
# Starts four processes in the correct order:
#   1. pylabhub-hubshell --dev   (broker; ephemeral keypair, no password)
#   2. pylabhub-producer producer/   (produces lab.demo.counter at 10 Hz)
#   3. pylabhub-processor processor/ (reads counter, writes lab.demo.processed)
#   4. pylabhub-consumer consumer/   (reads lab.demo.processed; foreground)
#
# Ctrl-C stops all processes cleanly via the cleanup trap.
#
# Usage:
#   bash share/demo/run_demo.sh [--build-type Debug|Release]
#
# Prerequisites:
#   cmake --build build   (builds all targets)
#
# To swap a script: edit the relevant __init__.py and re-run.  No recompile needed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Parse arguments ────────────────────────────────────────────────────────
BUILD_TYPE="Debug"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--build-type Debug|Release]"
            echo ""
            echo "Pipeline:"
            echo "  hubshell (broker) → producer → processor → consumer"
            echo ""
            echo "Press Ctrl-C to stop all processes."
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

BIN_DIR="${REPO_ROOT}/build/stage-${BUILD_TYPE}/bin"

# ── Verify binaries ────────────────────────────────────────────────────────
for BIN in pylabhub-hubshell pylabhub-producer pylabhub-processor pylabhub-consumer; do
    if [[ ! -x "${BIN_DIR}/${BIN}" ]]; then
        echo "ERROR: ${BIN_DIR}/${BIN} not found or not executable." >&2
        echo "  Run: cmake --build build" >&2
        exit 1
    fi
done

# ── PIDs for cleanup ───────────────────────────────────────────────────────
HUB_PID=""
PRODUCER_PID=""
PROC_PID=""

cleanup() {
    echo ""
    echo "[demo] shutting down..."
    for PID_VAR in PROC_PID PRODUCER_PID HUB_PID; do
        eval "PID=\$$PID_VAR"
        if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
            kill -SIGTERM "${PID}" 2>/dev/null || true
            wait "${PID}" 2>/dev/null || true
            echo "[demo] ${PID_VAR%%_PID} stopped (pid=${PID})"
        fi
    done
    echo "[demo] done."
}
trap cleanup EXIT INT TERM

# ── 1. Start hub (dev mode — no hub directory or password required) ────────
echo "[demo] Starting hub (--dev mode, broker at tcp://127.0.0.1:5570)..."
"${BIN_DIR}/pylabhub-hubshell" --dev &
HUB_PID=$!
sleep 0.5

if ! kill -0 "${HUB_PID}" 2>/dev/null; then
    echo "ERROR: hubshell exited immediately — port 5570 may be in use." >&2
    HUB_PID=""
    exit 1
fi
echo "[demo] hub running (pid=${HUB_PID})"

# ── 2. Start producer ──────────────────────────────────────────────────────
echo "[demo] Starting producer (lab.demo.counter @ 10 Hz)..."
"${BIN_DIR}/pylabhub-producer" "${SCRIPT_DIR}/producer" &
PRODUCER_PID=$!
sleep 0.6   # wait for SHM creation + broker registration

if ! kill -0 "${PRODUCER_PID}" 2>/dev/null; then
    echo "ERROR: producer exited immediately — check logs." >&2
    PRODUCER_PID=""
    exit 1
fi
echo "[demo] producer running (pid=${PRODUCER_PID})"

# ── 3. Start processor ─────────────────────────────────────────────────────
echo "[demo] Starting processor (counter → processed)..."
"${BIN_DIR}/pylabhub-processor" "${SCRIPT_DIR}/processor" &
PROC_PID=$!
sleep 0.6   # wait for consumer + producer SHM connections

if ! kill -0 "${PROC_PID}" 2>/dev/null; then
    echo "ERROR: processor exited immediately — check logs." >&2
    PROC_PID=""
    exit 1
fi
echo "[demo] processor running (pid=${PROC_PID})"

# ── 4. Start consumer (foreground) ────────────────────────────────────────
echo "[demo] Starting consumer (lab.demo.processed)... (Ctrl-C to stop)"
echo "[demo] Output: count | ts | value | doubled | rate"
echo ""
"${BIN_DIR}/pylabhub-consumer" "${SCRIPT_DIR}/consumer"
# Consumer exits first → cleanup trap fires
