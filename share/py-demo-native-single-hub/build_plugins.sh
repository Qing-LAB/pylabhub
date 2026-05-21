#!/bin/bash
# Build native plugins for the demo.  Run by demo.manifest.json
# setup_commands before plh_hub/plh_role start.
#
# Uses the include dir staged next to the binary on PATH (the runner
# arranges PATH = stage-debug/bin + system).
set -euo pipefail

# $1 = the demo runner's $BIN_DIR (passed via manifest setup_commands).
# Fallback: try to resolve via PATH if no arg given (for manual runs).
if [[ $# -ge 1 && -n "$1" ]]; then
    BIN_DIR="$1"
else
    BIN_DIR=$(dirname "$(realpath "$(command -v plh_hub)")")
fi
INCLUDE_DIR="$BIN_DIR/../include"

if [[ ! -f "$INCLUDE_DIR/utils/native_engine_api.h" ]]; then
    echo "ERROR: native_engine_api.h not found at $INCLUDE_DIR/utils/" >&2
    echo "       (looked relative to plh_hub at $BIN_DIR)" >&2
    exit 1
fi

echo "Building native plugin: producer/script/native/plugin.so"
g++ -shared -fPIC -std=c++20 -O2 \
    -I"$INCLUDE_DIR" \
    -o producer/script/native/plugin.so \
    producer/script/native/plugin.cpp

echo "Plugin built successfully."
ls -la producer/script/native/plugin.so
