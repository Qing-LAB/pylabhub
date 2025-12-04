#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   assemble_xop_wrapper.sh <src_root> <build_root> <xop_name> [<config>]
#
# Example:
#   ./scripts/assemble_xop_wrapper.sh /path/to/repo /path/to/build pylabhubxop Release
#
# Notes:
#  - <xop_name> should be the base name you use in CMake (e.g. "pylabhubxop").
#  - This wrapper looks for the built module as:
#      <build_root>/src/IgorXOP/<CONFIG>/<xop_name>64
#      <build_root>/src/IgorXOP/<CONFIG>/<xop_name>64.xop
#      <build_root>/src/IgorXOP/<CONFIG>/<xop_name>64.xop.bundle  (existing bundle)
#    It will pick the first match. If it finds a bundle it will call the assemble script
#    to ensure Info.plist is present and resources are copied.
#
#  - The wrapper passes:
#      -D_build_root (the build root)
#      -D_mach_o    (the found built target path or bundle)
#      -D_bundle_dir (destination bundle dir; ends with .xop)
#      -D_xop_name  (bundle base name, e.g. pylabhubxop64)
#      -D_config    (build config, e.g. Release)
#
#  - The assemble_xop.cmake will create the <bundle>.xop folder if required.

SRC_ROOT="$1"
BUILD_ROOT="$2"
XOP_NAME="$3"
CONFIG="${4:-Release}"

# Derived
XOP_BASE="${XOP_NAME}64"                           # e.g. pylabhubxop64
BUILD_SUBDIR="${BUILD_ROOT}/src/IgorXOP/${CONFIG}"
MACH_O_CAND1="${BUILD_SUBDIR}/${XOP_BASE}"         # plain module
MACH_O_CAND2="${BUILD_SUBDIR}/${XOP_BASE}.xop"     # single-file xop produced by linker
BUNDLE_CAND="${BUILD_SUBDIR}/${XOP_BASE}.xop.bundle" # already assembled bundle (rare)
BUNDLE_DIR="${BUILD_SUBDIR}/${XOP_BASE}.xop"      # desired bundle dir to produce
ASSEMBLE_SCRIPT="${SRC_ROOT}/cmake/assemble_xop.cmake"

echo "assemble_xop_wrapper.sh: SRC_ROOT=${SRC_ROOT}"
echo "assemble_xop_wrapper.sh: BUILD_ROOT=${BUILD_ROOT}"
echo "assemble_xop_wrapper.sh: XOP_NAME=${XOP_NAME}"
echo "assemble_xop_wrapper.sh: XOP_BASE=${XOP_BASE}"
echo "assemble_xop_wrapper.sh: CONFIG=${CONFIG}"
echo "assemble_xop_wrapper.sh: looking for candidates in ${BUILD_SUBDIR}"

# Check assemble script exists
if [ ! -f "${ASSEMBLE_SCRIPT}" ]; then
  echo "ERROR: assemble script not found: ${ASSEMBLE_SCRIPT}" >&2
  exit 3
fi

# Find the built artifact
found=""
if [ -f "${MACH_O_CAND1}" ]; then
  found="${MACH_O_CAND1}"
  echo "assemble_xop_wrapper.sh: found module (no suffix): ${found}"
elif [ -f "${MACH_O_CAND2}" ]; then
  found="${MACH_O_CAND2}"
  echo "assemble_xop_wrapper.sh: found module (with .xop file): ${found}"
elif [ -d "${BUNDLE_CAND}" ]; then
  found="${BUNDLE_CAND}"
  echo "assemble_xop_wrapper.sh: found already-assembled bundle: ${found}"
else
  # try fuzzy search (sometimes Xcode output name differs slightly)
  echo "assemble_xop_wrapper.sh: primary candidates not found, trying pattern search..."
  # look for files starting with XOP_BASE in build dir
  mapfile -t matches < <(ls -1 "${BUILD_SUBDIR}" 2>/dev/null | grep -E "^${XOP_BASE}(\.xop|)$" || true)
  if [ "${#matches[@]}" -gt 0 ]; then
    candidate="${BUILD_SUBDIR}/${matches[0]}"
    if [ -f "${candidate}" ] || [ -d "${candidate}" ]; then
      found="${candidate}"
      echo "assemble_xop_wrapper.sh: pattern matched: ${found}"
    fi
  fi
fi

if [ -z "${found}" ]; then
  echo "ERROR: could not find built module in any expected location." >&2
  echo "  Tried: ${MACH_O_CAND1}, ${MACH_O_CAND2}, ${BUNDLE_CAND}" >&2
  exit 2
fi

# Invoke cmake packaging script using -D arguments (assemble_xop.cmake expects these)
# -D_mach_o: path to built module OR existing bundle directory
# -D_bundle_dir: destination bundle directory (we ensure .xop suffix)
# -D_xop_name: bundle base name (no .xop suffix)
# -D_build_root: build root (where Info.plist was configured during CMake)
# -D_config: build configuration

# Ensure bundle dir ends with .xop
bundle_dir="${BUNDLE_DIR}"
case "${bundle_dir}" in
  *.xop) ;;
  *) bundle_dir="${bundle_dir}.xop" ;;
esac

echo "assemble_xop_wrapper.sh: invoking assemble script:"
echo "  ASSEMBLE_SCRIPT = ${ASSEMBLE_SCRIPT}"
echo "  _mach_o = ${found}"
echo "  _bundle_dir = ${bundle_dir}"
echo "  _xop_name = ${XOP_BASE}"
echo "  _build_root = ${BUILD_ROOT}"
echo "  _config = ${CONFIG}"

cmake -D_build_root="${BUILD_ROOT}" \
      -D_mach_o="${found}" \
      -D_bundle_dir="${bundle_dir}" \
      -D_xop_name="${XOP_BASE}" \
      -D_config="${CONFIG}" \
      -P "${ASSEMBLE_SCRIPT}"

echo "assemble_xop_wrapper.sh: assemble script finished."

exit 0
