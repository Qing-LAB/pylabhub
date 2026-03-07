#!/usr/bin/env bash
# run_demo.sh — pylabhub dual-hub bridge demo
#
# Topology (6 processes):
#
#   Hub A (5570)                                         Hub B (5571)
#   Producer ─[SHM]─► Processor-A ─[ZMQ PUSH bind]──►
#                      registers:                 tcp://127.0.0.1:5580
#                      data_transport=zmq ──────► Processor-B ─[SHM]─► Consumer
#                      zmq_node_endpoint          (discovers endpoint
#                      in Hub A broker            via Hub A broker —
#                                                 HEP-CORE-0021)
#
# Processor-B uses HEP-CORE-0021: it connects to Hub A's broker (in_hub_dir)
# and discovers the ZMQ endpoint from the DISC_ACK — no hardcoded address.
#
# Usage:
#   bash share/demo-dual-hub/run_demo.sh [--build-type Debug|Release]
#
# Ctrl-C stops all 6 processes cleanly via the cleanup trap.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Parse arguments ────────────────────────────────────────────────────────
BUILD_TYPE="debug"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--build-type debug|release]"
            echo ""
            echo "Topology:"
            echo "  Hub A (5570) → Producer [SHM] → Processor-A [ZMQ PUSH]"
            echo "  Hub B (5571) ← Consumer [SHM] ← Processor-B [ZMQ PULL]"
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
PIDS=()

cleanup() {
    echo ""
    echo "[demo] shutting down..."
    for PID in "${PIDS[@]}"; do
        if kill -0 "${PID}" 2>/dev/null; then
            kill -SIGTERM "${PID}" 2>/dev/null || true
        fi
    done
    for PID in "${PIDS[@]}"; do
        wait "${PID}" 2>/dev/null || true
    done
    echo "[demo] done."
}
trap cleanup EXIT INT TERM

# ── 1. Start Hub A ────────────────────────────────────────────────────────
echo "[demo] Starting Hub A (broker at tcp://0.0.0.0:5570)..."
"${BIN_DIR}/pylabhub-hubshell" "${SCRIPT_DIR}/hub-a" --dev &
PIDS+=($!)
sleep 0.5

if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
    echo "ERROR: Hub A exited immediately — port 5570 may be in use." >&2
    exit 1
fi
echo "[demo] Hub A running (pid=${PIDS[-1]})"

# ── 2. Start Hub B ────────────────────────────────────────────────────────
echo "[demo] Starting Hub B (broker at tcp://0.0.0.0:5571)..."
"${BIN_DIR}/pylabhub-hubshell" "${SCRIPT_DIR}/hub-b" --dev &
PIDS+=($!)
sleep 0.5

if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
    echo "ERROR: Hub B exited immediately — port 5571 may be in use." >&2
    exit 1
fi
echo "[demo] Hub B running (pid=${PIDS[-1]})"

# ── 3. Start Producer (publishes on Hub A) ────────────────────────────────
echo "[demo] Starting Producer (lab.bridge.raw @ 10 Hz on Hub A)..."
"${BIN_DIR}/pylabhub-producer" "${SCRIPT_DIR}/producer" &
PIDS+=($!)
sleep 0.6

if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
    echo "ERROR: Producer exited immediately." >&2
    exit 1
fi
echo "[demo] Producer running (pid=${PIDS[-1]})"

# ── 4. Start Processor-A (SHM in → ZMQ PUSH out) ─────────────────────────
echo "[demo] Starting Processor-A (SHM → ZMQ PUSH tcp://127.0.0.1:5580)..."
"${BIN_DIR}/pylabhub-processor" "${SCRIPT_DIR}/processor-a" &
PIDS+=($!)
sleep 0.6

if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
    echo "ERROR: Processor-A exited immediately." >&2
    exit 1
fi
echo "[demo] Processor-A running (pid=${PIDS[-1]})"

# ── 5. Start Processor-B (ZMQ PULL in → SHM out) ─────────────────────────
echo "[demo] Starting Processor-B (ZMQ PULL → SHM on Hub B)..."
"${BIN_DIR}/pylabhub-processor" "${SCRIPT_DIR}/processor-b" &
PIDS+=($!)
sleep 0.6

if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
    echo "ERROR: Processor-B exited immediately." >&2
    exit 1
fi
echo "[demo] Processor-B running (pid=${PIDS[-1]})"

# ── 6. Start Consumer (foreground, reads from Hub B) ──────────────────────
echo "[demo] Starting Consumer (lab.bridge.processed from Hub B)..."
echo "[demo] Output: count | ts | value | doubled | rate"
echo "[demo] Press Ctrl-C to stop all 6 processes."
echo ""
"${BIN_DIR}/pylabhub-consumer" "${SCRIPT_DIR}/consumer"
# Consumer exits first → cleanup trap fires
