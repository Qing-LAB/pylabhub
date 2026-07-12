#!/usr/bin/env bash
#
# ctest_evidence.sh — ctest wrapper that always preserves failure logs.
#
# **Why this exists.**  ctest's default behavior:
# 1. `LastTest.log` and `LastTestsFailed.log` under `build/Testing/Temporary/`
#    are OVERWRITTEN by every subsequent ctest invocation, including
#    `--rerun-failed`.  A "flake" that passes on rerun destroys the
#    failure evidence.
# 2. Fixture-based artifact preservation
#    (`build/stage-*/test_artifacts/`) requires the test process to run
#    to teardown.  A crash before teardown loses those artifacts too.
# This wrapper closes both holes: every invocation writes a
# **timestamped**, never-overwritten log under `build/Testing/logs/`
# AND backs up `LastTest.log` + `LastTestsFailed.log` before ctest
# runs.  When a test fails, the failure evidence is on disk and stays
# on disk even if the operator reruns before reading it.
#
# Usage — the same argument set you'd pass to ctest:
#
#   tools/ctest_evidence.sh -L layer4 -j 2
#   tools/ctest_evidence.sh --rerun-failed
#   tools/ctest_evidence.sh -R '^PlhHubCliTest\.ZmqE2E_' -V
#
# The script cd's into `build/` (or `$PYLABHUB_BUILD_DIR` if set) and
# passes ALL args through to ctest.  It ADDS `--output-on-failure`
# unless already present.  It ADDS `--output-log <timestamped-path>`.
#
# **Rule (mirrors HEP-CORE-0011 §Session hygiene + `feedback_
# read_log_before_rerun`):** After a failure, READ the timestamped log
# BEFORE any rerun.  This wrapper cannot force that discipline, but it
# guarantees the evidence is available to read.
#
# See mirror `tools/ctest_evidence.ps1` for Windows.

set -eu

BUILD_DIR="${PYLABHUB_BUILD_DIR:-$(cd "$(dirname "$0")/.." && pwd)/build}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "FAIL — build dir '$BUILD_DIR' not found." >&2
    echo "Set PYLABHUB_BUILD_DIR or run from repo root with 'build/' present." >&2
    exit 2
fi

LOGS_DIR="$BUILD_DIR/Testing/logs"
mkdir -p "$LOGS_DIR"

# Nanosecond timestamp — plain seconds (%Y%m%d-%H%M%S) collides under
# parallel invocations of this wrapper (same second, same shell PID
# is impossible but different shells can race).  %N gives us
# nanoseconds on GNU date; on BSD/macOS `date` doesn't support %N so
# we fall back to appending a random 4-digit suffix.  Either path
# gives per-invocation uniqueness.
if TS=$(date +%Y%m%d-%H%M%S-%N 2>/dev/null) && \
   [ "${TS: -1}" != "N" ]; then
    :  # GNU date path — %N expanded successfully
else
    TS="$(date +%Y%m%d-%H%M%S)-$(awk 'BEGIN{srand();printf "%04d", rand()*10000}')"
fi
PID=$$
LOG_PATH="$LOGS_DIR/ctest-${TS}-pid${PID}.log"
BACKUP_LAST="$LOGS_DIR/LastTest-preserved-${TS}-pid${PID}.log"
BACKUP_FAILED="$LOGS_DIR/LastTestsFailed-preserved-${TS}-pid${PID}.log"

# STEP 1 — Back up prior LastTest.log + LastTestsFailed.log BEFORE
# invoking ctest.  Preserves evidence from the run that produced the
# CURRENT failure state so a `--rerun-failed` cannot destroy it.
# Backup failure is FATAL: a silent evidence-preservation failure is
# exactly the anti-pattern this wrapper exists to prevent, so we
# abort loudly rather than proceed with a false sense of safety.
if [ -f "$BUILD_DIR/Testing/Temporary/LastTest.log" ]; then
    if ! cp -p "$BUILD_DIR/Testing/Temporary/LastTest.log" "$BACKUP_LAST"; then
        echo "FAIL [ctest_evidence] could not back up LastTest.log to $BACKUP_LAST" >&2
        echo "  Refusing to run ctest — a silent evidence-preservation failure is the" >&2
        echo "  exact anti-pattern this wrapper exists to prevent." >&2
        exit 3
    fi
    echo "[ctest_evidence] backed up LastTest.log -> $BACKUP_LAST" >&2
fi
if [ -f "$BUILD_DIR/Testing/Temporary/LastTestsFailed.log" ]; then
    if ! cp -p "$BUILD_DIR/Testing/Temporary/LastTestsFailed.log" "$BACKUP_FAILED"; then
        echo "FAIL [ctest_evidence] could not back up LastTestsFailed.log to $BACKUP_FAILED" >&2
        exit 3
    fi
    echo "[ctest_evidence] backed up LastTestsFailed.log -> $BACKUP_FAILED" >&2
fi

# STEP 2 — Assemble ctest args.  Preserve caller args verbatim, add
# `--output-on-failure` if not present, add `--output-log` at a
# timestamped path.
declare -a ARGS
HAS_OUTPUT_ON_FAILURE=0
HAS_OUTPUT_LOG=0
for a in "$@"; do
    case "$a" in
        --output-on-failure) HAS_OUTPUT_ON_FAILURE=1 ;;
        --output-log|--output-log=*) HAS_OUTPUT_LOG=1 ;;
    esac
    ARGS+=("$a")
done
if [ "$HAS_OUTPUT_ON_FAILURE" -eq 0 ]; then ARGS+=("--output-on-failure"); fi
if [ "$HAS_OUTPUT_LOG" -eq 0 ]; then ARGS+=("--output-log" "$LOG_PATH"); fi

echo "[ctest_evidence] evidence log -> $LOG_PATH" >&2
echo "[ctest_evidence] running: ctest ${ARGS[*]}" >&2

# STEP 3 — Run ctest with our augmented args.  cd into build dir first
# so ctest finds CTestTestfile.cmake without --test-dir.
cd "$BUILD_DIR"
set +e
ctest "${ARGS[@]}"
CTEST_RC=$?
set -e

# STEP 4 — Report evidence locations to stderr.  If ctest failed and
# the wrapper's log file exists + has content, print a pointer to it.
if [ "$CTEST_RC" -ne 0 ]; then
    echo "" >&2
    echo "[ctest_evidence] ctest exited $CTEST_RC — evidence preserved:" >&2
    if [ -s "$LOG_PATH" ]; then
        echo "  ctest output log:            $LOG_PATH" >&2
    fi
    if [ -f "$BUILD_DIR/Testing/Temporary/LastTest.log" ]; then
        echo "  fresh LastTest.log:          $BUILD_DIR/Testing/Temporary/LastTest.log" >&2
    fi
    if [ -f "$BUILD_DIR/Testing/Temporary/LastTestsFailed.log" ]; then
        echo "  fresh LastTestsFailed.log:   $BUILD_DIR/Testing/Temporary/LastTestsFailed.log" >&2
    fi
    if [ -s "$BACKUP_LAST" ]; then
        echo "  prior-run LastTest backup:   $BACKUP_LAST" >&2
    fi
    if [ -s "$BACKUP_FAILED" ]; then
        echo "  prior-run failed-tests bkp:  $BACKUP_FAILED" >&2
    fi
    echo "" >&2
    echo "[ctest_evidence] READ THE LOG BEFORE ANY --rerun-failed.  See" >&2
    echo "  docs/HEP/HEP-CORE-0011 § Session hygiene + feedback_read_log_before_rerun." >&2
fi

exit "$CTEST_RC"
