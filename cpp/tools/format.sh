#!/usr/bin/env bash
# cpp/tools/format.sh
# --------------------------------------------------------------------
# Formats C/C++ sources (clang-format) and CMake files (cmake-format).
# - Only scans include/, src/, tests/ directories.
# - Excludes third_party/ and .git/.
# - Displays each file it checks/formats.
# - Usage:
#     ./cpp/tools/format.sh           # format in-place
#     ./cpp/tools/format.sh --check   # check-only (exit 1 if any file would change)
#     ./cpp/tools/format.sh --quiet   # suppress per-file printing
# --------------------------------------------------------------------

set -euo pipefail
IFS=$'\n\t'

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Directories to scan (relative to ROOT). Edit if you want different folders.
SCAN_DIRS=( "${ROOT}/include" "${ROOT}/src" "${ROOT}/tests" )

# Exclude patterns (paths containing these fragments will be skipped)
EXCLUDE_PATH_FRAGMENTS=( "third_party" ".git" "_build" "build")

# Options
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

for arg in "$@"; do
  case "$arg" in
    --check) CHECK_MODE=1 ;;
    --quiet) SHOW_FILES=0 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $arg"; usage; exit 2 ;;
  esac
done

echo "---------------------------------------------"
echo "Format script root: $ROOT"
echo "Check mode: $CHECK_MODE"
echo "Show per-file: $SHOW_FILES"
echo "---------------------------------------------"

# Portable finder for C/C++ files (excludes paths containing exclude fragments)
find_cpp_files_under() {
  local dir="$1"
  [ -d "$dir" ] || return 0

  # Build -not -path predicates for exclude fragments
  local find_excludes=()
  for frag in "${EXCLUDE_PATH_FRAGMENTS[@]}"; do
    find_excludes+=( -not -path "*/${frag}/*" )
  done

  # Use grouped -iname patterns for portability
  find "$dir" -type f "${find_excludes[@]}" \
    \( -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' -o \
       -iname '*.h' -o -iname '*.hh' -o -iname '*.hpp' -o -iname '*.hxx' \) -print
}

find_cmake_files_under() {
  local dir="$1"
  [ -d "$dir" ] || return 0

  local find_excludes=()
  for frag in "${EXCLUDE_PATH_FRAGMENTS[@]}"; do
    find_excludes+=( -not -path "*/${frag}/*" )
  done

  find "$dir" -type f "${find_excludes[@]}" \( -iname 'CMakeLists.txt' -o -iname '*.cmake' \) -print
}

# Collect files
CXX_FILES=()
CMAKE_FILES=()

for d in "${SCAN_DIRS[@]}"; do
  while IFS= read -r f; do
    CXX_FILES+=("$f")
  done < <(find_cpp_files_under "$d")

  while IFS= read -r f; do
    CMAKE_FILES+=("$f")
  done < <(find_cmake_files_under "$d")
done

# Also check for top-level CMake files (root), excluding third_party
while IFS= read -r f; do
  # Avoid duplicating if already picked up
  [[ " ${CMAKE_FILES[*]} " == *" $f "* ]] || CMAKE_FILES+=("$f")
done < <(find_cmake_files_under "$ROOT")

# Helper: print count and early exit if none
echo "Found ${#CXX_FILES[@]} C/C++ files to consider."
echo "Found ${#CMAKE_FILES[@]} CMake files to consider."

if [ ${#CXX_FILES[@]} -eq 0 ] && [ ${#CMAKE_FILES[@]} -eq 0 ]; then
  echo "Nothing to format. Exiting."
  exit 0
fi

# ---------- clang-format step ----------
if ! command -v clang-format >/dev/null 2>&1; then
  echo "ERROR: clang-format not found. Please install clang-format."
  exit 2
fi

if [ ${#CXX_FILES[@]} -gt 0 ]; then
  if [ "$CHECK_MODE" -eq 1 ]; then
    echo
    echo "clang-format: CHECK mode — comparing formatted output (no file modifications)..."
    TMPDIR="$(mktemp -d)"
    STATUS=0
    for f in "${CXX_FILES[@]}"; do
      [ "$SHOW_FILES" -eq 1 ] && printf "Checking: %s\n" "$f"
      # produce formatted output to a temp file and diff
      clang-format "$f" > "$TMPDIR/formatted.tmp"
      if ! diff -q "$f" "$TMPDIR/formatted.tmp" >/dev/null 2>&1; then
        printf "Needs formatting: %s\n" "$f"
        STATUS=1
      fi
    done
    rm -rf "$TMPDIR"
    if [ "$STATUS" -ne 0 ]; then
      echo "clang-format: Some files need formatting. Run the script without --check to apply changes."
      exit 1
    else
      echo "clang-format: All C/C++ files are properly formatted."
    fi
  else
    echo
    echo "clang-format: Formatting files in-place..."
    for f in "${CXX_FILES[@]}"; do
      [ "$SHOW_FILES" -eq 1 ] && printf "Formatting: %s\n" "$f"
      clang-format -i "$f"
    done
    echo "clang-format: Done."
  fi
fi

# ---------- cmake-format step ----------
if command -v cmake-format >/dev/null 2>&1; then
  if [ ${#CMAKE_FILES[@]} -gt 0 ]; then
    if [ "$CHECK_MODE" -eq 1 ]; then
      echo
      echo "cmake-format: CHECK mode — comparing formatted output (no file modifications)..."
      TMPDIR="$(mktemp -d)"
      STATUS=0
      for f in "${CMAKE_FILES[@]}"; do
        [ "$SHOW_FILES" -eq 1 ] && printf "Checking: %s\n" "$f"
        cmake-format --config-file "${ROOT}/cmake-format.yaml" "$f" > "$TMPDIR/formatted.tmp" 2>/dev/null || {
          echo "Warning: cmake-format failed on $f (skipping)"; continue
        }
        if ! diff -q "$f" "$TMPDIR/formatted.tmp" >/dev/null 2>&1; then
          printf "Needs formatting: %s\n" "$f"
          STATUS=1
        fi
      done
      rm -rf "$TMPDIR"
      if [ "$STATUS" -ne 0 ]; then
        echo "cmake-format: Some files need formatting. Run the script without --check to apply changes."
        exit 1
      else
        echo "cmake-format: All CMake files are properly formatted."
      fi
    else
      echo
      echo "cmake-format: Formatting files in-place..."
      for f in "${CMAKE_FILES[@]}"; do
        [ "$SHOW_FILES" -eq 1 ] && printf "Formatting: %s\n" "$f"
        cmake-format -i --config-file "${ROOT}/cmake-format.yaml" "$f" 2>/dev/null || {
          echo "Warning: cmake-format failed on $f (skipping)"; continue
        }
      done
      echo "cmake-format: Done."
    fi
  fi
else
  echo
  echo "cmake-format not found; skipping formatting of CMake files. (pip install cmake-format)"
fi

echo
echo "---------------------------------------------"
echo "Formatting complete."
echo "---------------------------------------------"
exit 0
