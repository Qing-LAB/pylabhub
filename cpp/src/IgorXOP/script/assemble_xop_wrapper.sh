#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   assemble_xop_wrapper.sh <src_root> <build_root> <xop_name>
SRC_ROOT="$1"
BUILD_ROOT="$2"
XOP_NAME="$3"

CONFIG=${CONFIGURATION:-Release}

MACH_O="${BUILD_ROOT}/src/IgorXOP/${CONFIG}/${XOP_NAME}64.xop"
BUNDLE_DIR="${BUILD_ROOT}/src/IgorXOP/${CONFIG}/${XOP_NAME}64.xop.bundle"
ASSEMBLE_SCRIPT="${SRC_ROOT}/cmake/assemble_xop.cmake"

echo "assemble_xop_wrapper.sh: CONFIG=${CONFIG}"
echo "assemble_xop_wrapper.sh: looking for mach-o -> ${MACH_O}"

if [ ! -f "${MACH_O}" ]; then
  echo "ERROR: mach-o not found at ${MACH_O}" >&2
  exit 2
fi

cmake -P "${ASSEMBLE_SCRIPT}" "${MACH_O}" "${BUNDLE_DIR}" "${XOP_NAME}.xop"
