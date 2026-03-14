#!/usr/bin/env bash
# tools/format.sh
# --------------------------------------------------------------------
# Formats C/C++ sources (clang-format) and CMake files (cmake-format).
# - Scans directories listed in SCAN_DIRS (defaults: include/, src/, tests/).
# - Excludes paths listed in EXCLUDE_PATH_FRAGMENTS or .formatignore.
# - Supports --check and --quiet.
# - Safe handling of filenames via -print0 and read -d ''.
# - Portable across Linux, macOS and FreeBSD.
# --------------------------------------------------------------------

set -euo pipefail
IFS=$'\n\t'

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# -------------------------
# Configuration (editable)
# -------------------------
SCAN_DIRS=( "include" "src" "tests" )

# Exclude fragments to prune (fast). We want to prune third_party but allow a
# very small explicit whitelist under it (handled in a separate pass).
EXCLUDE_PATH_FRAGMENTS=( "third_party" ".git" "_build" "build" )

# Explicit exceptions (precise). We'll run a small second find pass (no pruning)
# to collect only these paths:
# - any file exactly matching */third_party/CMakeLists.txt
# - any file under */third_party/cmake/** (recursive)
EXPLICIT_EXCEPTIONS=( "third_party/CMakeLists.txt" "third_party/cmake" )

FORMAT_IGNORE_FILE="${ROOT}/.formatignore"

# Behavior flags (can be overridden via env)
FORCE_TOP_LEVEL=${FORCE_TOP_LEVEL:-1}   # if 1, clang-format will be forced to use top-level .clang-format
CHECK_MODE=0
SHOW_FILES=1

usage() {
  cat <<EOF
Usage: $(basename "$0") [--check] [--quiet] [-h|--help]

  --check   : verify formatting (no files modified). Exit 1 if any file needs formatting.
  --quiet   : suppress per-file output (still prints summary).
  -h|--help : show this help
EOF
}

# -------------------------
# Parse args
# -------------------------
for arg in "$@"; do
  case "$arg" in
    --check) CHECK_MODE=1 ;;
    --quiet) SHOW_FILES=0 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $arg"; usage; exit 2 ;;
  esac
done

# -------------------------
# Portable mkdtemp helper
# -------------------------
portable_mkdtemp() {
  local d
  if d=$(mktemp -d 2>/dev/null); then
    printf '%s' "$d"
    return 0
  fi
  if d=$(mktemp -d -t format 2>/dev/null); then
    printf '%s' "$d"
    return 0
  fi
  d="/tmp/format.$$"
  mkdir -p "$d"
  printf '%s' "$d"
}

# -------------------------
# Build final exclude list (allow !negation in .formatignore optionally ignored)
# -------------------------
EXCLUDES=()
if [ ${#EXCLUDE_PATH_FRAGMENTS[@]} -gt 0 ]; then
  for frag in "${EXCLUDE_PATH_FRAGMENTS[@]}"; do
    EXCLUDES+=("${frag%/}")
  done
fi

# If .formatignore exists, append non-empty, non-comment lines (ignore '!' negation lines here
# to keep explicit exception logic deterministic; you may adapt if you want to support '!' there).
if [ -f "$FORMAT_IGNORE_FILE" ]; then
  while IFS= read -r line || [ -n "$line" ]; do
    # strip leading/trailing whitespace
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" ]] && continue
    [[ "$line" =~ ^# ]] && continue
    # Ignore negation lines in .formatignore for simplicity; keep config in-script.
    if [[ "$line" == '!'* ]]; then
      continue
    fi
    EXCLUDES+=("$line")
  done < "$FORMAT_IGNORE_FILE"
fi

# -------------------------
# Build find prune tokens (always prune EXCLUDES)
# -------------------------
_build_prune_tokens() {
  local frag
  local out=""
  for frag in "${EXCLUDES[@]}"; do
    out="${out} -path '*/${frag}' -o -path '*/${frag}/*' -o"
  done
  if [ -n "$out" ]; then
    out="${out% -o}"
  fi
  printf '%s' "$out"
}
PRUNE_TOKENS="$(_build_prune_tokens)"

# -------------------------
# Finder helpers (first pass: prune excludes)
# Each collector prints NUL-separated list of matches
# -------------------------
collect_cpp_files_pruned() {
  local root_dir="$1"
  [ -d "$root_dir" ] || return 0

  if [ -n "$PRUNE_TOKENS" ]; then
    eval "find \"${root_dir}\" \( ${PRUNE_TOKENS} \) -prune -o -type f \
      \( -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \
         -o -iname '*.h' -o -iname '*.hh' -o -iname '*.hpp' -o -iname '*.hxx' \) -print0"
  else
    find "$root_dir" -type f \
      \( -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \
         -o -iname '*.h' -o -iname '*.hh' -o -iname '*.hpp' -o -iname '*.hxx' \) -print0
  fi
}

collect_cmake_files_pruned() {
  local root_dir="$1"
  [ -d "$root_dir" ] || return 0

  if [ -n "$PRUNE_TOKENS" ]; then
    eval "find \"${root_dir}\" \( ${PRUNE_TOKENS} \) -prune -o -type f \
      \( -iname 'CMakeLists.txt' -o -iname '*.cmake' \) -print0"
  else
    find "$root_dir" -type f \( -iname 'CMakeLists.txt' -o -iname '*.cmake' \) -print0
  fi
}

# -------------------------
# First pass: collect files with pruning (fast)
# -------------------------
CXX_FILES=()
CMAKE_FILES=()

for d in "${SCAN_DIRS[@]}"; do
  full="${ROOT}/${d}"
  if [ -d "$full" ]; then
    while IFS= read -r -d '' f; do CXX_FILES+=("$f"); done < <(collect_cpp_files_pruned "$full")
    while IFS= read -r -d '' f; do CMAKE_FILES+=("$f"); done < <(collect_cmake_files_pruned "$full")
  fi
done

# Also include top-level CMake files from ROOT
while IFS= read -r -d '' f; do
  case " ${CMAKE_FILES[*]} " in
    *" $f "*) : ;; # already present
    *) CMAKE_FILES+=("$f") ;;
  esac
done < <(collect_cmake_files_pruned "$ROOT")

# -------------------------
# Second pass: collect explicit exceptions (no pruning)
# Only add the precise exception paths we want under third_party.
# -------------------------
_collect_exception_files_no_prune() {
  # This function collects files matching the EXPLICIT_EXCEPTIONS patterns under ROOT.
  # It prints NUL-delimited matches to stdout.
  local pat
  local find_expr=""
  for pat in "${EXPLICIT_EXCEPTIONS[@]}"; do
    # normalize pat (strip leading/trailing slashes)
    pat="${pat#/}"; pat="${pat%/}"
    if [[ "$pat" == */* ]]; then
      # If pattern refers to a directory (e.g. third_party/cmake) we want the subtree:
      # match either the directory itself (rare for files) or any file under it.
      find_expr="${find_expr} -path '*/${pat}/*' -o -path '*/${pat}' -o"
    else
      # Not expected (we keep precise patterns only), but include as a filename fragment
      find_expr="${find_expr} -path '*/${pat}' -o -path '*/${pat}/*' -o"
    fi
  done

  if [ -n "$find_expr" ]; then
    # remove trailing -o
    find_expr="${find_expr% -o}"
    # Use eval to expand the tokenized expression safely.
    eval "find \"${ROOT}\" -type f \( ${find_expr} \) -print0"
  fi
}

# Run exception collector and append to CMAKE_FILES (these are CMake exception paths).
# We only append matches that look like CMake files (CMakeLists.txt or *.cmake)
while IFS= read -r -d '' f; do
  case "$f" in
    */CMakeLists.txt|*.cmake)
      CMAKE_FILES+=("$f")
      ;;
    *)
      # ignore non-cmake exceptions (shouldn't occur given our EXPLICIT_EXCEPTIONS)
      ;;
  esac
done < <(_collect_exception_files_no_prune)

# -------------------------
# Dedupe file lists (simple portable dedupe)
# -------------------------
_dedupe_array() {
  # args: name of source array; will print NUL-delimded unique list
  local src_name="$1"
  eval "local -a src=(\"\${${src_name}[@]}\")"
  local seen_file
  # Use temporary file to hold seen keys (portable). We'll print NUL-delimited output.
  # But to keep it simple and portable, we'll use a bash loop with a string-based 'seen' list.
  local out=()
  local f
  for f in "${src[@]}"; do
    # Use simple dedupe by membership test in out (works for typical repo sizes)
    local found=0
    for seen_file in "${out[@]}"; do
      if [ "$seen_file" = "$f" ]; then found=1; break; fi
    done
    if [ "$found" -eq 0 ]; then
      out+=("$f")
    fi
  done
  # Print as NUL-delimited so callers can read easily
  for f in "${out[@]}"; do printf '%s\0' "$f"; done
}

# Replace arrays with deduped versions
CXX_FILES_NEW=()
while IFS= read -r -d '' f; do CXX_FILES_NEW+=("$f"); done < <(_dedupe_array CXX_FILES)
CMAKE_FILES_NEW=()
while IFS= read -r -d '' f; do CMAKE_FILES_NEW+=("$f"); done < <(_dedupe_array CMAKE_FILES)
CXX_FILES=("${CXX_FILES_NEW[@]}")
CMAKE_FILES=("${CMAKE_FILES_NEW[@]}")

# -------------------------
# Report
# -------------------------
echo "---------------------------------------------"
echo "Format script root: $ROOT"
echo "Check mode: $CHECK_MODE"
echo "Show per-file: $SHOW_FILES"
echo "Total C/C++ files found: ${#CXX_FILES[@]}"
echo "Total CMake files found:   ${#CMAKE_FILES[@]}"
echo "Pruned excludes:"
for e in "${EXCLUDES[@]}"; do echo "  - $e"; done
echo "Explicit exceptions added (exact):"
for e in "${EXPLICIT_EXCEPTIONS[@]}"; do echo "  - $e"; done
echo "---------------------------------------------"

if [ ${#CXX_FILES[@]} -eq 0 ] && [ ${#CMAKE_FILES[@]} -eq 0 ]; then
  echo "Nothing to format. Exiting."
  exit 0
fi

# -------------------------
# clang-format step (unchanged logic)
# -------------------------
if command -v clang-format >/dev/null 2>&1; then
  STYLE_ARG="-style=file"

  # detect support for --dry-run -Werror
  CAN_DRY_RUN=0
  if clang-format --help 2>&1 | grep -q -- --dry-run && clang-format --help 2>&1 | grep -q -- -Werror; then
    CAN_DRY_RUN=1
  fi

  if [ ${#CXX_FILES[@]} -gt 0 ]; then
    if [ "$CHECK_MODE" -eq 1 ]; then
      echo
      echo "clang-format: CHECK mode..."
      STATUS=0
      if [ "${CAN_DRY_RUN}" -eq 1 ]; then
        for f in "${CXX_FILES[@]}"; do
          [ "$SHOW_FILES" -eq 1 ] && printf "Checking: %s\n" "$f"
          if [ "${FORCE_TOP_LEVEL}" -eq 1 ]; then
            ASSUME_ARG="--assume-filename=${ROOT}/$(basename "$f")"
          else
            ASSUME_ARG=""
          fi
          if ! clang-format --dry-run -Werror ${STYLE_ARG} ${ASSUME_ARG} "$f" 2>/dev/null; then
            printf "Needs formatting: %s\n" "$f"
            STATUS=1
          fi
        done
      else
        TMPDIR="$(portable_mkdtemp)"
        for f in "${CXX_FILES[@]}"; do
          [ "$SHOW_FILES" -eq 1 ] && printf "Checking: %s\n" "$f"
          if [ "${FORCE_TOP_LEVEL}" -eq 1 ]; then
            ASSUME_ARG="--assume-filename=${ROOT}/$(basename "$f")"
          else
            ASSUME_ARG=""
          fi
          clang-format ${STYLE_ARG} ${ASSUME_ARG} "$f" > "$TMPDIR/formatted.tmp" 2>/dev/null || true
          if ! diff -q "$f" "$TMPDIR/formatted.tmp" >/dev/null 2>&1; then
            printf "Needs formatting: %s\n" "$f"
            STATUS=1
          fi
        done
        rm -rf "$TMPDIR"
      fi

      if [ "$STATUS" -ne 0 ]; then
        echo "clang-format: Some files need formatting."
        exit 1
      else
        echo "clang-format: All C/C++ files are properly formatted."
      fi
    else
      echo
      echo "clang-format: Formatting files in-place..."
      for f in "${CXX_FILES[@]}"; do
        [ "$SHOW_FILES" -eq 1 ] && printf "Formatting: %s\n" "$f"
        if [ "${FORCE_TOP_LEVEL}" -eq 1 ]; then
          ASSUME_ARG="--assume-filename=${ROOT}/$(basename "$f")"
        else
          ASSUME_ARG=""
        fi
        if ! clang-format -i ${STYLE_ARG} ${ASSUME_ARG} "$f" 2> >(tee /dev/stderr >/dev/null); then
          echo "Warning: clang-format failed for $f"
        fi
      done
      echo "clang-format: Done."
    fi
  fi
else
  echo "clang-format not found; skipping C/C++ formatting. (install clang-format)"
fi

# -------------------------
# cmake-format step (unchanged logic)
# -------------------------
if command -v cmake-format >/dev/null 2>&1; then
  CMAKE_FORMAT_CONFIG_OPT="--config-files"
  if ! cmake-format --help 2>&1 | grep -q -- '--config-files'; then
    if cmake-format --help 2>&1 | grep -q -- '--config-file'; then
      CMAKE_FORMAT_CONFIG_OPT="--config-file"
    else
      CMAKE_FORMAT_CONFIG_OPT=""
    fi
  fi
  if [ -n "$CMAKE_FORMAT_CONFIG_OPT" ] && [ -f "${ROOT}/cmake-format.yaml" ]; then
    CMAKE_FORMAT_CONFIG_ARG=("$CMAKE_FORMAT_CONFIG_OPT" "${ROOT}/cmake-format.yaml")
    echo "cmake-format: Using config ${ROOT}/cmake-format.yaml"
  else
    CMAKE_FORMAT_CONFIG_ARG=()
  fi

  if [ ${#CMAKE_FILES[@]} -gt 0 ]; then
    if [ "$CHECK_MODE" -eq 1 ]; then
      echo
      echo "cmake-format: CHECK mode..."
      TMPDIR="$(portable_mkdtemp)"
      STATUS=0

      set +e
      if cmake-format "${CMAKE_FORMAT_CONFIG_ARG[@]}" --dump-config >/dev/null 2>&1; then
        cmake-format "${CMAKE_FORMAT_CONFIG_ARG[@]}" --dump-config | sed -n '1,60p'
      fi
      set -e

      for f in "${CMAKE_FILES[@]}"; do
        [ "$SHOW_FILES" -eq 1 ] && printf "Checking: %s\n" "$f"
        if ! cmake-format "${CMAKE_FORMAT_CONFIG_ARG[@]}" "$f" > "$TMPDIR/formatted.tmp" 2> "$TMPDIR/cmake-format.err"; then
          echo "Warning: cmake-format failed on $f (see $TMPDIR/cmake-format.err). Skipping."
          cat "$TMPDIR/cmake-format.err" >&2 || true
          continue
        fi
        if ! diff -q "$f" "$TMPDIR/formatted.tmp" >/dev/null 2>&1; then
          printf "Needs formatting: %s\n" "$f"
          STATUS=1
        fi
      done
      rm -rf "$TMPDIR"

      if [ "$STATUS" -ne 0 ]; then
        echo "cmake-format: Some CMake files need formatting."
        exit 1
      else
        echo "cmake-format: All CMake files are properly formatted."
      fi
    else
      echo
      echo "cmake-format: Formatting files in-place..."
      for f in "${CMAKE_FILES[@]}"; do
        [ "$SHOW_FILES" -eq 1 ] && printf "Formatting: %s\n" "$f"
        if ! cmake-format -i "${CMAKE_FORMAT_CONFIG_ARG[@]}" "$f" 2> >(tee /dev/stderr >/dev/null); then
          echo "Warning: cmake-format failed on $f (skipping)"
          continue
        fi
      done
      echo "cmake-format: Done."
    fi
  fi
else
  echo "cmake-format not found; skipping CMake formatting. (pip install cmake-format)"
fi

echo
echo "---------------------------------------------"
echo "Formatting complete."
echo "---------------------------------------------"
exit 0
