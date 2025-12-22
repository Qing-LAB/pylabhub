#!/usr/bin/env bash
# tools/pack_source.sh
# --------------------------------------------------------------------
# Archives project source files into a gzipped tarball.
#
# - Uses 'git ls-files' to reliably list all tracked files.
# - Excludes directories like 'third_party/', build directories, and IDE folders.
# - Packages C/C++ sources, CMake files, docs, and other tracked assets
#   into a tarball with a versioned root directory.
# --------------------------------------------------------------------

set -euo pipefail
IFS=$'\n\t'

# --- Default Configuration ---
# The script determines the project root by going up one level from its own location.
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="$PROJECT_ROOT"
PROJECT_NAME=$(basename "$PROJECT_ROOT")
OUTPUT_FILE="${PROJECT_ROOT}/${PROJECT_NAME}-src.tar.gz"

# --- Usage Message ---
usage() {
  cat <<EOF
Usage: $(basename "$0") [-o <output_file>] [-d <source_dir>] [-h|--help]

Packs project source files into a .tar.gz archive.
This script relies on 'git ls-files' and must be run within a Git repository.

Options:
  -o, --output    Specify the output archive file path.
                  (Default: ${PROJECT_ROOT}/${PROJECT_NAME}-src.tar.gz)
  -d, --dir       The source directory to archive. Must be the project root.
                  (Default: ${PROJECT_ROOT})
  -h, --help      Show this help message.
EOF
}

# --- Argument Parsing ---
while [[ $# -gt 0 ]]; do
  case "$1" in
    -o|--output)
      # Ensure output path is absolute
      if [[ "$2" != /* ]]; then
        OUTPUT_FILE="$(pwd)/$2"
      else
        OUTPUT_FILE="$2"
      fi
      shift 2
      ;;
    -d|--dir)
      SOURCE_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

# --- Pre-run Checks ---
cd "$SOURCE_DIR"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Error: Source directory '$SOURCE_DIR' is not a git repository." >&2
  exit 1
fi

if [[ "$SOURCE_DIR" != "$PROJECT_ROOT" ]]; then
    echo "Warning: The specified source directory is different from the auto-detected project root."
    echo "  - Script location implies root: $PROJECT_ROOT"
    echo "  - Provided source directory:    $SOURCE_DIR"
fi


# --- Main Logic ---
ARCHIVE_ROOT_DIR="${PROJECT_NAME}-src"

echo "--------------------------------------------------"
echo "Project root:    $SOURCE_DIR"
echo "Output archive:  $OUTPUT_FILE"
echo "Archive root dir: $ARCHIVE_ROOT_DIR"
echo "--------------------------------------------------"

# Define patterns to exclude from the archive.
# Uses regex compatible with grep -E.
# Anchored to the start of the path (relative to git root).
EXCLUDE_PATTERNS=(
  '^third_party/'
  '^build/'
  '^\.git'
  '^\.idea/'
  '^\.vscode/'
  '^cmake-build-debug/'
  '\.zip$'
  '\.tar\.gz$'
  '\.tgz$'
)

# Combine patterns into a single regex for grep
GREP_EXCLUDE_REGEX=$(IFS=\|; echo "${EXCLUDE_PATTERNS[*]}")

echo "Collecting files to archive..."

# Get the null-delimited list of files directly from git and grep.
# This stream will be used for both counting and archiving.
# Store it in a temporary file to avoid running git/grep twice.
# Or, better, just run git/grep twice if the user's filesets are not huge.
# For simplicity and robustness against shell variable issues, I will run it twice.

# First, get the count
file_count=$(git ls-files -z | grep -vazE "$GREP_EXCLUDE_REGEX" | tr -dc '\0' | wc -c | xargs || echo "0")

if [[ "$file_count" -eq 0 ]]; then
    echo "No files found to archive after applying exclusions. Aborting."
    exit 1
fi

echo "Found $file_count files. Creating archive..."

# Second, create the archive by piping the list directly to tar.
# Define the prefix modification based on OS
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS (BSD tar) uses -s
    TRANSFORM_FLAG=("-s" ",^,${ARCHIVE_ROOT_DIR}/,")
else
    # Linux (GNU tar) uses --transform
    TRANSFORM_FLAG=("--transform" "s,^,${ARCHIVE_ROOT_DIR}/,")
fi

git ls-files -z | grep -vazE "$GREP_EXCLUDE_REGEX" | tar -czvf "$OUTPUT_FILE" \
    --null \
    --files-from - \
    "${TRANSFORM_FLAG[@]}"


if [ $? -eq 0 ]; then
  echo "--------------------------------------------------"
  echo "Archive created successfully:"
  echo "  => $OUTPUT_FILE"
  echo "--------------------------------------------------"
else
  echo "--------------------------------------------------" >&2
  echo "Error: Archive creation failed." >&2
  echo "--------------------------------------------------" >&2
  exit 1
fi

exit 0
