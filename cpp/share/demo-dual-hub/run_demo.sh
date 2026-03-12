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
BUILD_TYPE="Debug"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--build-type Debug|Release]"
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

REQUIRED_BINS=(pylabhub-hubshell pylabhub-producer pylabhub-processor pylabhub-consumer)

is_valid_bin_dir() {
    local d="$1"
    [[ -d "${d}" ]] || return 1
    for b in "${REQUIRED_BINS[@]}"; do
        [[ -x "${d}/${b}" ]] || return 1
    done
    return 0
}

BUILD_TYPE_LC="$(printf '%s' "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"
BUILD_TYPE_UC="$(printf '%s' "${BUILD_TYPE_LC}" | tr '[:lower:]' '[:upper:]')"

BIN_DIR=""
for CAND in \
    "${REPO_ROOT}/build/stage-${BUILD_TYPE}/bin" \
    "${REPO_ROOT}/build/stage-${BUILD_TYPE_LC}/bin" \
    "${REPO_ROOT}/build/stage-${BUILD_TYPE_UC}/bin" \
    "${REPO_ROOT}/build/bin"
do
    if is_valid_bin_dir "${CAND}"; then
        BIN_DIR="${CAND}"; break
    fi
done

if [[ -z "${BIN_DIR}" ]]; then
    echo "ERROR: build binaries not found for build type '${BUILD_TYPE}'." >&2
    echo "  Run: cmake --build ${REPO_ROOT}/build" >&2
    exit 1
fi

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
echo "[demo] Starting Hub A (broker at tcp://127.0.0.1:5570)..."
"${BIN_DIR}/pylabhub-hubshell" "${SCRIPT_DIR}/hub-a" --dev &
PIDS+=($!)
sleep 0.5

if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
    echo "ERROR: Hub A exited immediately — port 5570 may be in use." >&2
    exit 1
fi
echo "[demo] Hub A running (pid=${PIDS[-1]})"

# ── 2. Start Hub B ────────────────────────────────────────────────────────
echo "[demo] Starting Hub B (broker at tcp://127.0.0.1:5571)..."
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
