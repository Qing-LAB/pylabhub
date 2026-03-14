#!/usr/bin/env bash
# run_demo.sh — pylabhub end-to-end pipeline demo
#
# Starts four processes in the correct order:
#   1. pylabhub-hubshell hub/ --dev   (broker; ephemeral keypair, no password)
#   2. pylabhub-producer producer/   (produces lab.demo.counter at max rate: 1k float32/slot)
#   3. pylabhub-processor processor/ (doubles payload, writes lab.demo.processed)
#   4. pylabhub-consumer consumer/   (prints throughput once per second)
#
# Ctrl-C stops all processes cleanly via the cleanup trap.
#
# Usage:
#   bash share/py-demo-single-processor-shm/run_demo.sh [--build-dir DIR] [--build-type Debug|Release]
#
# Prerequisites:
#   cmake --build build   (builds all targets)
#
# To swap a script: edit the relevant __init__.py and re-run.  No recompile needed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SCHEMA_DIR="${SCRIPT_DIR}/hub/schemas/lab/demo"

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
            echo "Pipeline:"
            echo "  hubshell (broker) → producer → processor → consumer"
            echo ""
            echo "Press Ctrl-C to stop all processes."
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── Ensure named schema directory/files exist (derived from demo scripts) ───
mkdir -p "${SCHEMA_DIR}"

if [[ ! -f "${SCHEMA_DIR}/counter.v1.json" ]]; then
    cat > "${SCHEMA_DIR}/counter.v1.json" <<'JSON'
{
  "id": "lab.demo.counter",
  "version": 1,
  "description": "Throughput demo input stream: counter + timestamp + 1k float32 payload",
  "slot": {
    "packing": "aligned",
    "fields": [
      {"name": "count", "type": "int64"},
      {"name": "ts", "type": "float64"},
      {"name": "samples", "type": "float32", "count": 1024}
    ]
  }
}
JSON
fi

if [[ ! -f "${SCHEMA_DIR}/processed.v1.json" ]]; then
    cat > "${SCHEMA_DIR}/processed.v1.json" <<'JSON'
{
  "id": "lab.demo.processed",
  "version": 1,
  "description": "Throughput demo processed stream: samples doubled, scale factor appended",
  "slot": {
    "packing": "aligned",
    "fields": [
      {"name": "count",   "type": "int64"},
      {"name": "ts",      "type": "float64"},
      {"name": "samples", "type": "float32", "count": 1024},
      {"name": "scale",   "type": "float32"}
    ]
  }
}
JSON
fi

# ── Resolve binaries ───────────────────────────────────────────────────────
BUILD_TYPE_LC="$(printf '%s' "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"
BUILD_TYPE_UC="$(printf '%s' "${BUILD_TYPE_LC}" | tr '[:lower:]' '[:upper:]')"

REQUIRED_BINS=(
    "pylabhub-hubshell"
    "pylabhub-producer"
    "pylabhub-processor"
    "pylabhub-consumer"
)

is_valid_bin_dir() {
    local d="$1"
    [[ -d "${d}" ]] || return 1
    for b in "${REQUIRED_BINS[@]}"; do
        [[ -x "${d}/${b}" ]] || return 1
    done
    return 0
}

# Default build directory to "build" if not specified
if [[ -z "${BUILD_DIR}" ]]; then
    BUILD_DIR="build"
fi

# Resolve to absolute path (relative paths are relative to REPO_ROOT)
if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}"
fi

BIN_DIR=""
CANDIDATES=(
    "${BUILD_DIR}/stage-${BUILD_TYPE}/bin"
    "${BUILD_DIR}/stage-${BUILD_TYPE_LC}/bin"
    "${BUILD_DIR}/stage-${BUILD_TYPE_UC}/bin"
    "${BUILD_DIR}/bin"
)

for CAND in "${CANDIDATES[@]}"; do
    if is_valid_bin_dir "${CAND}"; then
        BIN_DIR="${CAND}"
        break
    fi
done

if [[ -z "${BIN_DIR}" ]]; then
    while IFS= read -r MATCH; do
        if is_valid_bin_dir "${MATCH}"; then
            BIN_DIR="${MATCH}"
            break
        fi
    done < <(find "${BUILD_DIR}" -maxdepth 3 -type d -path "*/stage-*/bin" 2>/dev/null | sort)
fi

# ── Verify binaries ────────────────────────────────────────────────────────
for BIN in "${REQUIRED_BINS[@]}"; do
    if [[ ! -x "${BIN_DIR}/${BIN}" ]]; then
        echo "ERROR: ${BIN} not found in expected build output." >&2
        echo "  Requested build type: ${BUILD_TYPE}" >&2
        echo "  Checked bin dir: ${BIN_DIR:-<none>}" >&2
        echo "  Run: cmake --build ${BUILD_DIR}" >&2
        exit 1
    fi
done

echo "[demo] Using binaries from: ${BIN_DIR}"

# ── Log directory ────────────────────────────────────────────────────────
LOG_DIR="${SCRIPT_DIR}/logs"
mkdir -p "${LOG_DIR}"
echo "[demo] Log files: ${LOG_DIR}/"

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

# ── 1. Start hub (dev mode + demo hub_dir for broker metadata/keys) ───────
echo "[demo] Starting hub (--dev mode, broker at tcp://127.0.0.1:5570)..."
"${BIN_DIR}/pylabhub-hubshell" "${SCRIPT_DIR}/hub" --dev < /dev/null &
HUB_PID=$!
sleep 0.5

if ! kill -0 "${HUB_PID}" 2>/dev/null; then
    echo "ERROR: hubshell exited immediately — port 5570 may be in use." >&2
    HUB_PID=""
    exit 1
fi
echo "[demo] hub running (pid=${HUB_PID})"

# ── 2. Start producer ──────────────────────────────────────────────────────
echo "[demo] Starting producer (lab.demo.counter @ max rate)..."
"${BIN_DIR}/pylabhub-producer" "${SCRIPT_DIR}/producer" --log-file "${LOG_DIR}/producer.log" < /dev/null &
PRODUCER_PID=$!
sleep 2.0   # wait for CurveZMQ handshake + SHM creation + broker registration

if ! kill -0 "${PRODUCER_PID}" 2>/dev/null; then
    echo "ERROR: producer exited immediately — check logs." >&2
    PRODUCER_PID=""
    exit 1
fi
echo "[demo] producer running (pid=${PRODUCER_PID})"

# ── 3. Start processor ─────────────────────────────────────────────────────
echo "[demo] Starting processor (counter → processed)..."
"${BIN_DIR}/pylabhub-processor" "${SCRIPT_DIR}/processor" --log-file "${LOG_DIR}/processor.log" < /dev/null &
PROC_PID=$!
sleep 2.0   # wait for CurveZMQ handshake + consumer + producer SHM connections

if ! kill -0 "${PROC_PID}" 2>/dev/null; then
    echo "ERROR: processor exited immediately — check logs." >&2
    PROC_PID=""
    exit 1
fi
echo "[demo] processor running (pid=${PROC_PID})"

# ── 4. Start consumer (foreground) ────────────────────────────────────────
echo "[demo] Starting consumer (lab.demo.processed)..."
echo "[demo] Output: once-per-second throughput (slots/s, MiB/s)"
echo "[demo] Auto-stop: consumer stops after 100000 slots; channel_closing cascades to all roles."
echo ""
"${BIN_DIR}/pylabhub-consumer" "${SCRIPT_DIR}/consumer" --log-file "${LOG_DIR}/consumer.log"
# Consumer exits first → cleanup trap fires
