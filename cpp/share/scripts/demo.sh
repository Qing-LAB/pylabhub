#!/usr/bin/env bash
# demo.sh — run the pylabhub counter demo (broker + producer + consumer)
#
# Run explicitly with bash (this script is intentionally not marked executable):
#   bash share/scripts/demo.sh [--build-type Debug|Release]
#
# For Windows use the companion PowerShell script:
#   powershell -ExecutionPolicy Bypass -File share\scripts\demo.ps1
#
# Prerequisites:
#   cmake --build build
#   (python-build-standalone env must already be prepared by cmake)

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
            echo "Starts pylabhub-broker, producer_counter, and consumer_logger."
            echo "Press Ctrl-C to stop all processes."
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

BIN_DIR="${REPO_ROOT}/build/stage-${BUILD_TYPE}/bin"

# ── Verify binaries ────────────────────────────────────────────────────────────
for BIN in pylabhub-broker pylabhub-actor; do
    if [[ ! -x "${BIN_DIR}/${BIN}" ]]; then
        echo "ERROR: ${BIN_DIR}/${BIN} not found or not executable." >&2
        echo "Run: cmake --build build --target ${BIN}" >&2
        exit 1
    fi
done

for CFG in producer_counter.json consumer_logger.json; do
    if [[ ! -f "${EXAMPLES_DIR}/${CFG}" ]]; then
        echo "ERROR: ${EXAMPLES_DIR}/${CFG} not found." >&2
        exit 1
    fi
done

# ── PIDs to clean up ─────────────────────────────────────────────────────────
BROKER_PID=""
PRODUCER_PID=""

cleanup() {
    echo ""
    echo "[demo] shutting down..."
    if [[ -n "${PRODUCER_PID}" ]] && kill -0 "${PRODUCER_PID}" 2>/dev/null; then
        kill -SIGTERM "${PRODUCER_PID}" 2>/dev/null || true
        wait "${PRODUCER_PID}" 2>/dev/null || true
        echo "[demo] producer stopped (pid=${PRODUCER_PID})"
    fi
    if [[ -n "${BROKER_PID}" ]] && kill -0 "${BROKER_PID}" 2>/dev/null; then
        kill -SIGTERM "${BROKER_PID}" 2>/dev/null || true
        wait "${BROKER_PID}" 2>/dev/null || true
        echo "[demo] broker stopped (pid=${BROKER_PID})"
    fi
    echo "[demo] done."
}
trap cleanup EXIT INT TERM

# ── Start broker ─────────────────────────────────────────────────────────────
echo "[demo] starting pylabhub-broker..."
"${BIN_DIR}/pylabhub-broker" &
BROKER_PID=$!
sleep 0.4   # wait for broker to bind on tcp://127.0.0.1:5570

if ! kill -0 "${BROKER_PID}" 2>/dev/null; then
    echo "ERROR: broker exited immediately — check logs." >&2
    BROKER_PID=""
    exit 1
fi
echo "[demo] broker running (pid=${BROKER_PID})"

# ── Start producer ────────────────────────────────────────────────────────────
echo "[demo] starting producer_counter..."
"${BIN_DIR}/pylabhub-actor" \
    --config "${EXAMPLES_DIR}/producer_counter.json" &
PRODUCER_PID=$!
sleep 0.6   # wait for producer SHM creation + broker registration

if ! kill -0 "${PRODUCER_PID}" 2>/dev/null; then
    echo "ERROR: producer exited immediately — check logs." >&2
    PRODUCER_PID=""
    exit 1
fi
echo "[demo] producer running (pid=${PRODUCER_PID})"

# ── Start consumer (foreground) ───────────────────────────────────────────────
echo "[demo] starting consumer_logger... (Ctrl-C to stop)"
echo ""
"${BIN_DIR}/pylabhub-actor" \
    --config "${EXAMPLES_DIR}/consumer_logger.json"
# consumer exits → cleanup trap fires
