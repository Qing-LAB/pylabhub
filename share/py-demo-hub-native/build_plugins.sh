#!/bin/bash
set -euo pipefail
if [[ $# -ge 1 && -n "$1" ]]; then
    BIN_DIR="$1"
else
    BIN_DIR=$(dirname "$(realpath "$(command -v plh_hub)")")
fi
INCLUDE_DIR="$BIN_DIR/../include"
if [[ ! -f "$INCLUDE_DIR/utils/native_engine_api.h" ]]; then
    echo "ERROR: native_engine_api.h not found at $INCLUDE_DIR/utils/" >&2
    exit 1
fi
echo "Building hub/script/native/plugin.so"
g++ -shared -fPIC -std=c++20 -O2 \
    -I"$INCLUDE_DIR" \
    -o hub/script/native/plugin.so \
    hub/script/native/plugin.cpp
ls -la hub/script/native/plugin.so
