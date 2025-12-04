#!/usr/bin/env bash
# tools/format.sh
# --------------------------------------------------------------------
# Formats C/C++ sources (clang-format) and CMake files (cmake-format).
# - Scans directories listed in SCAN_DIRS (defaults: include/, src/, tests/).
# - Excludes paths listed in EXCLUDE_PATH_FRAGMENTS or .formatignore.
# - Supports --check and --quiet.
# - Safe handling of filenames via -print0 and read -d ''.
# --------------------------------------------------------------------

set -euo pipefail
IFS=$'\n\t'

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# -------------------------
# Configuration (editable)
# -------------------------
# Directories to scan (absolute paths will be constructed using ROOT)
SCAN_DIRS=( "include" "src" "tests" )

# Built-in exclude fragments (substring matched anywhere in path)
EXCLUDE_PATH_FRAGMENTS=( "third_party" ".git" "_build" "build" )

# Optional: a repo-level ignore file (one path fragment or path per line)
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
You can also set environment variables:
  FORCE_TOP_LEVEL=1  # force using ROOT/.clang-format
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
# Build final exclude list
# -------------------------
# Start with built-in fragments
EXCLUDES=("${EXCLUDE_PATH_FRAGMENTS[@]}")

# If .formatignore exists, append non-empty, non-comment lines
if [ -f "$FORMAT_IGNORE_FILE" ]; then
  while IFS= read -r line || [ -n "$line" ]; do
    # strip leading/trailing whitespace
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    # skip empty/comment
    [[ -z "$line" ]] && continue
    [[ "$line" =~ ^# ]] && continue
    EXCLUDES+=("$line")
  done < "$FORMAT_IGNORE_FILE"
fi

# Helper: build find-style -path predicates that match a fragment anywhere in the path.
# Example: fragment "third_party" produces -path "*/third_party" -o -path "*/third_party/*"
_build_find_exclude_args() {
  local dir="$1"   # root directory we will scan, used to optionally create anchored patterns (not required)
  local -n out_arr="$2"
  out_arr=()
  for frag in "${EXCLUDES[@]}"; do
    # if frag looks like an absolute or relative path (contains /), match that path segment as-is
    # otherwise match any path component containing this fragment.
    out_arr+=( -path "*/${frag}" -o -path "*/${frag}/*" -o )
  done
  # remove trailing -o if present
  if [ ${#out_arr[@]} -gt 0 ]; then
    unset 'out_arr[${#out_arr[@]}-1]'
  fi
}

# -------------------------
# Finder helpers
# -------------------------
collect_cpp_files() {
  local root_dir="$1"
  local -n result="$2"
  result=()
  [ -d "$root_dir" ] || return 0

  local find_excl=()
  _build_find_exclude_args "$root_dir" find_excl

  # Use -print0 for safe handling of special characters and spaces
  if [ ${#find_excl[@]} -gt 0 ]; then
    while IFS= read -r -d '' f; do result+=("$f"); done < <(
      find "$root_dir" \( "${find_excl[@]}" \) -prune -o -type f \
        \( -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \
           -o -iname '*.h' -o -iname '*.hh' -o -iname '*.hpp' -o -iname '*.hxx' \) -print0
    )
  else
    while IFS= read -r -d '' f; do result+=("$f"); done < <(
      find "$root_dir" -type f \
        \( -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \
           -o -iname '*.h' -o -iname '*.hh' -o -iname '*.hpp' -o -iname '*.hxx' \) -print0
    )
  fi
}

collect_cmake_files() {
  local root_dir="$1"
  local -n result="$2"
  result=()
  [ -d "$root_dir" ] || return 0

  local find_excl=()
  _build_find_exclude_args "$root_dir" find_excl

  if [ ${#find_excl[@]} -gt 0 ]; then
    while IFS= read -r -d '' f; do result+=("$f"); done < <(
      find "$root_dir" \( "${find_excl[@]}" \) -prune -o -type f \
        \( -iname 'CMakeLists.txt' -o -iname '*.cmake' \) -print0
    )
  else
    while IFS= read -r -d '' f; do result+=("$f"); done < <(
      find "$root_dir" -type f \( -iname 'CMakeLists.txt' -o -iname '*.cmake' \) -print0
    )
  fi
}

# -------------------------
# Collect files
# -------------------------
CXX_FILES=()
CMAKE_FILES=()

for d in "${SCAN_DIRS[@]}"; do
  full="${ROOT}/${d}"
  collect_cpp_files "$full" out_cpp
  # append
  for f in "${out_cpp[@]}"; do CXX_FILES+=("$f"); done

  collect_cmake_files "$full" out_cmake
  for f in "${out_cmake[@]}"; do CMAKE_FILES+=("$f"); done
done

# Also include top-level CMake files (root) - useful for top-level CMakeLists.txt
collect_cmake_files "$ROOT" top_cmake
for f in "${top_cmake[@]}"; do
  # dedupe: only add if not already present
  case " ${CMAKE_FILES[*]} " in
    *" $f "*) : ;; # already present
    *) CMAKE_FILES+=("$f") ;;
  esac
done

# Report counts
echo "---------------------------------------------"
echo "Format script root: $ROOT"
echo "Check mode: $CHECK_MODE"
echo "Show per-file: $SHOW_FILES"
echo "Total C/C++ files found: ${#CXX_FILES[@]}"
echo "Total CMake files found:   ${#CMAKE_FILES[@]}"
echo "Excludes (from builtin + .formatignore):"
for e in "${EXCLUDES[@]}"; do echo "  - $e"; done
echo "---------------------------------------------"

if [ ${#CXX_FILES[@]} -eq 0 ] && [ ${#CMAKE_FILES[@]} -eq 0 ]; then
  echo "Nothing to format. Exiting."
  exit 0
fi

# -------------------------
# clang-format step (replacement to use --assume-filename when FORCE_TOP_LEVEL)
# -------------------------
if command -v clang-format >/dev/null 2>&1; then
  # default: ask clang-format to use discovery
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
            # force discovery starting at ROOT by assuming filename placed in ROOT
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
        TMPDIR="$(mktemp -d)"
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
# cmake-format step
# -------------------------
if command -v cmake-format >/dev/null 2>&1; then
  # Detect supported CLI option for specifying config
  CMAKE_FORMAT_CONFIG_OPT="--config-files"   # preferred
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
      TMPDIR="$(mktemp -d)"
      STATUS=0

      # show top of dump-config for debugging
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
