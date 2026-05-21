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

for ROLE in producer consumer; do
    echo "Building $ROLE/script/native/plugin.so"
    g++ -shared -fPIC -std=c++20 -O2 \
        -I"$INCLUDE_DIR" \
        -o $ROLE/script/native/plugin.so \
        $ROLE/script/native/plugin.cpp
done

echo "All native plugins built."
ls -la producer/script/native/plugin.so consumer/script/native/plugin.so
