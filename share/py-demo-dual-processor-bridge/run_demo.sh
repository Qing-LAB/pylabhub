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
#   bash share/py-demo-dual-processor-bridge/run_demo.sh [--build-dir DIR] [--build-type Debug|Release]
#
# Ctrl-C stops all 6 processes cleanly via the cleanup trap.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Parse arguments ────────────────────────────────────────────────────────
BUILD_TYPE="Debug"
BUILD_DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--build-dir DIR] [--build-type Debug|Release]"
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

# Default build directory to "build" if not specified
if [[ -z "${BUILD_DIR}" ]]; then
    BUILD_DIR="build"
fi

# Resolve to absolute path (relative paths are relative to REPO_ROOT)
if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}"
fi

BIN_DIR=""
for CAND in \
    "${BUILD_DIR}/stage-${BUILD_TYPE}/bin" \
    "${BUILD_DIR}/stage-${BUILD_TYPE_LC}/bin" \
    "${BUILD_DIR}/stage-${BUILD_TYPE_UC}/bin" \
    "${BUILD_DIR}/bin"
do
    if is_valid_bin_dir "${CAND}"; then
        BIN_DIR="${CAND}"; break
    fi
done

if [[ -z "${BIN_DIR}" ]]; then
    echo "ERROR: build binaries not found in '${BUILD_DIR}' for build type '${BUILD_TYPE}'." >&2
    echo "  Run: cmake --build ${BUILD_DIR}" >&2
    exit 1
fi

echo "[demo] Using binaries from: ${BIN_DIR}"

# ── Log directory ────────────────────────────────────────────────────────
LOG_DIR="${SCRIPT_DIR}/logs"
mkdir -p "${LOG_DIR}"
echo "[demo] Log files: ${LOG_DIR}/"

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
echo "[demo] Starting Producer (lab.bridge.raw max_rate 1k-sample blocks on Hub A)..."
"${BIN_DIR}/pylabhub-producer" "${SCRIPT_DIR}/producer" --log-file "${LOG_DIR}/producer.log" &
PIDS+=($!)
sleep 2.0

if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
    echo "ERROR: Producer exited immediately." >&2
    exit 1
fi
echo "[demo] Producer running (pid=${PIDS[-1]})"

# ── 4. Start Processor-A (SHM in → ZMQ PUSH out) ─────────────────────────
echo "[demo] Starting Processor-A (SHM → ZMQ PUSH tcp://127.0.0.1:5580)..."
"${BIN_DIR}/pylabhub-processor" "${SCRIPT_DIR}/processor-a" --log-file "${LOG_DIR}/processor-a.log" &
PIDS+=($!)
sleep 2.0

if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
    echo "ERROR: Processor-A exited immediately." >&2
    exit 1
fi
echo "[demo] Processor-A running (pid=${PIDS[-1]})"

# ── 5. Start Processor-B (ZMQ PULL in → SHM out) ─────────────────────────
echo "[demo] Starting Processor-B (ZMQ PULL → SHM on Hub B)..."
"${BIN_DIR}/pylabhub-processor" "${SCRIPT_DIR}/processor-b" --log-file "${LOG_DIR}/processor-b.log" &
PIDS+=($!)
sleep 2.0

if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
    echo "ERROR: Processor-B exited immediately." >&2
    exit 1
fi
echo "[demo] Processor-B running (pid=${PIDS[-1]})"

# ── 6. Start Consumer (foreground, reads from Hub B) ──────────────────────
echo "[demo] Starting Consumer (lab.bridge.processed from Hub B)..."
echo "[demo] Output: slots/s | MiB/s | total | count | scale | overflow | wait_us | work_us | data"
echo "[demo] Press Ctrl-C to stop all 6 processes."
echo ""
"${BIN_DIR}/pylabhub-consumer" "${SCRIPT_DIR}/consumer" --log-file "${LOG_DIR}/consumer.log"
# Consumer exits first → cleanup trap fires
