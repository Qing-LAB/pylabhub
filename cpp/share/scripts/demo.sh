#!/usr/bin/env bash
# demo.sh — run the pylabhub counter demo (hub + producer + consumer)
#
# Run explicitly with bash:
#   bash share/scripts/demo.sh [--build-type Debug|Release]
#
# For Windows use the companion PowerShell script:
#   powershell -ExecutionPolicy Bypass -File share\scripts\demo.ps1
#
# Prerequisites:
#   cmake --build build
#
# Starts three processes in order:
#   1. pylabhub-hubshell --dev   (broker; ephemeral keypair, no password)
#   2. pylabhub-actor producer_counter/  (produces lab.examples.counter at max rate)
#   3. pylabhub-actor consumer_logger/   (reads + logs; foreground)
#
# Ctrl-C stops all processes cleanly via the cleanup trap.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
EXAMPLES_DIR="${SCRIPT_DIR}/python/examples"

# ── Parse args ────────────────────────────────────────────────────────────────
BUILD_TYPE="Debug"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--build-type Debug|Release]"
            echo ""
            echo "Starts pylabhub-hubshell, producer_counter, and consumer_logger."
            echo "Press Ctrl-C to stop all processes."
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

BIN_DIR="${REPO_ROOT}/build/stage-${BUILD_TYPE}/bin"

# ── Verify binaries ───────────────────────────────────────────────────────────
for BIN in pylabhub-hubshell pylabhub-actor; do
    if [[ ! -x "${BIN_DIR}/${BIN}" ]]; then
        echo "ERROR: ${BIN_DIR}/${BIN} not found or not executable." >&2
        echo "  Run: cmake --build build" >&2
        exit 1
    fi
done

# ── Verify example directories ────────────────────────────────────────────────
for DIR in producer_counter consumer_logger; do
    if [[ ! -f "${EXAMPLES_DIR}/${DIR}/actor.json" ]]; then
        echo "ERROR: ${EXAMPLES_DIR}/${DIR}/actor.json not found." >&2
        exit 1
    fi
done

# ── PIDs for cleanup ──────────────────────────────────────────────────────────
HUB_PID=""
PRODUCER_PID=""

cleanup() {
    echo ""
    echo "[demo] shutting down..."
    for PID_VAR in PRODUCER_PID HUB_PID; do
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

# ── 1. Start hub (dev mode — no hub directory or password required) ────────────
echo "[demo] Starting hub (--dev mode, broker at tcp://127.0.0.1:5570)..."
"${BIN_DIR}/pylabhub-hubshell" --dev &
HUB_PID=$!
sleep 0.4   # wait for broker to bind

if ! kill -0 "${HUB_PID}" 2>/dev/null; then
    echo "ERROR: hubshell exited immediately — port 5570 may be in use." >&2
    HUB_PID=""
    exit 1
fi
echo "[demo] hub running (pid=${HUB_PID})"

# ── 2. Start producer ─────────────────────────────────────────────────────────
echo "[demo] Starting producer_counter (lab.examples.counter)..."
"${BIN_DIR}/pylabhub-actor" "${EXAMPLES_DIR}/producer_counter" &
PRODUCER_PID=$!
sleep 0.6   # wait for SHM creation + broker registration

if ! kill -0 "${PRODUCER_PID}" 2>/dev/null; then
    echo "ERROR: producer exited immediately — check logs." >&2
    PRODUCER_PID=""
    exit 1
fi
echo "[demo] producer running (pid=${PRODUCER_PID})"

# ── 3. Start consumer (foreground) ────────────────────────────────────────────
echo "[demo] Starting consumer_logger... (Ctrl-C to stop)"
echo ""
"${BIN_DIR}/pylabhub-actor" "${EXAMPLES_DIR}/consumer_logger"
# Consumer exits first → cleanup trap fires
