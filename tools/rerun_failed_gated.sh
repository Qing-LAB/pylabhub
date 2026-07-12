#!/usr/bin/env bash
#
# rerun_failed_gated.sh — gated `ctest --rerun-failed` wrapper.
#
# **What this refuses to do without proof of read.**  Refuses to
# invoke `ctest --rerun-failed` unless a marker file
# `<build>/Testing/logs/.evidence-read` exists AND is newer than
# `<build>/Testing/Temporary/LastTest.log`.  The marker must be
# created via `read` sub-command below, which enforces that a
# non-empty summary text is provided.
#
# **Why**.  ctest --rerun-failed OVERWRITES `LastTest.log` — so a
# failing test that passes on rerun destroys the failure evidence.
# `feedback_read_log_before_rerun` warns against this; the memory
# is advisory and the reflex bypasses it.  This script is a HARD
# GATE that cannot be bypassed by momentum: raw `ctest --rerun-
# failed` is uncontrolled but `tools/rerun_failed_gated.sh` will
# not execute the rerun until the evidence-read marker is present.
#
# Usage:
#
#   # Attempt the rerun (rejected if marker is missing/stale):
#   tools/rerun_failed_gated.sh run
#
#   # Record that you've read the failure evidence.  Argument is a
#   # brief free-text summary of what the failure showed.  The
#   # summary is stored so a reviewer can see it later.
#   tools/rerun_failed_gated.sh read "hub log shows CURVE deny at
#   consumer's ZAP before producer's REG_ACK confirmed"
#
# Exit codes:
#   0 — action succeeded (run: rerun completed; read: marker written)
#   1 — refused (marker missing or stale on 'run'; summary missing on 'read')
#   2 — usage error

set -eu

BUILD_DIR="${PYLABHUB_BUILD_DIR:-$(cd "$(dirname "$0")/.." && pwd)/build}"
LOGS_DIR="$BUILD_DIR/Testing/logs"
MARKER="$LOGS_DIR/.evidence-read"
LAST_TEST="$BUILD_DIR/Testing/Temporary/LastTest.log"

usage() {
    echo "usage: $0 read <summary text>" >&2
    echo "       $0 run [extra ctest args...]" >&2
    exit 2
}

[ $# -lt 1 ] && usage
cmd="$1"
shift || true

case "$cmd" in
    read)
        if [ $# -lt 1 ] || [ -z "${1:-}" ]; then
            echo "FAIL — 'read' requires a non-empty summary argument." >&2
            echo "  Example:" >&2
            echo "  $0 read 'producer log shows checksum error at slot N=3'" >&2
            exit 1
        fi
        mkdir -p "$LOGS_DIR"
        {
            echo "read_at: $(date +%Y-%m-%dT%H:%M:%S%z)"
            echo "summary: $*"
            if [ -f "$LAST_TEST" ]; then
                echo "referenced_LastTest_mtime: $(stat -c '%y' "$LAST_TEST" 2>/dev/null || stat -f '%Sm' "$LAST_TEST")"
            fi
        } > "$MARKER"
        echo "[rerun_failed_gated] marker written: $MARKER" >&2
        exit 0
        ;;
    run)
        if [ ! -f "$MARKER" ]; then
            echo "REFUSED — evidence-read marker missing." >&2
            echo "  Expected: $MARKER" >&2
            echo "" >&2
            echo "  Read the failure evidence FIRST:" >&2
            [ -f "$LAST_TEST" ] && echo "    less $LAST_TEST" >&2
            [ -d "$BUILD_DIR/stage-debug/test_artifacts" ] && \
                echo "    ls $BUILD_DIR/stage-debug/test_artifacts/" >&2
            echo "" >&2
            echo "  Then record what you found:" >&2
            echo "    $0 read '<one-line summary of what the failure showed>'" >&2
            echo "" >&2
            echo "  Only after the marker is written may you rerun." >&2
            exit 1
        fi
        if [ -f "$LAST_TEST" ] && [ "$LAST_TEST" -nt "$MARKER" ]; then
            echo "REFUSED — evidence-read marker is STALE." >&2
            echo "  marker:    $MARKER" >&2
            echo "  LastTest:  $LAST_TEST (newer than marker)" >&2
            echo "" >&2
            echo "  A newer LastTest.log exists — a fresh run happened AFTER the last" >&2
            echo "  evidence read.  Re-read the current failure state and refresh the" >&2
            echo "  marker via '$0 read ...' before retrying." >&2
            exit 1
        fi
        BACKUP_TS="$(date +%Y%m%d-%H%M%S)"
        BACKUP="$LOGS_DIR/LastTest-pre-rerun-$BACKUP_TS.log"
        if [ -f "$LAST_TEST" ]; then
            mkdir -p "$LOGS_DIR"
            cp -p "$LAST_TEST" "$BACKUP"
            echo "[rerun_failed_gated] pre-rerun backup: $BACKUP" >&2
        fi
        cd "$BUILD_DIR"
        set +e
        ctest --rerun-failed --output-on-failure "$@"
        RC=$?
        set -e
        echo "" >&2
        echo "[rerun_failed_gated] rerun complete (exit $RC)." >&2
        echo "  pre-rerun log preserved: $BACKUP" >&2
        exit "$RC"
        ;;
    *)
        usage
        ;;
esac
