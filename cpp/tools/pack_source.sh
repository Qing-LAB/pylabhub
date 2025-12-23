#!/usr/bin/env bash
# tools/pack_source.sh
# --------------------------------------------------------------------
# Archives project source files into a gzipped tarball.
#
# - Uses 'git ls-files' to reliably list all tracked files.
# - Excludes most of third_party/ except:
#     * third_party/CMakeLists.txt
#     * third_party/cmake/**
# - Excludes build directories, IDE folders, and common archive files.
# - Packages files into a tarball with a versioned root directory.
# --------------------------------------------------------------------

set -euo pipefail
IFS=$'\n\t'

# --- Default Configuration ---
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="${PROJECT_ROOT}"
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
      if [[ -z "${2-}" ]]; then
        echo "Missing argument for $1" >&2
        usage; exit 1
      fi
      # Ensure output path is absolute
      if [[ "$2" != /* ]]; then
        OUTPUT_FILE="$(pwd)/$2"
      else
        OUTPUT_FILE="$2"
      fi
      shift 2
      ;;
    -d|--dir)
      if [[ -z "${2-}" ]]; then
        echo "Missing argument for $1" >&2
        usage; exit 1
      fi
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
# NOTE: Do NOT exclude whole third_party/ here — we will apply a strict allowlist for third_party below.
EXCLUDE_PATTERNS=(
  '^build/'
  '^\.(git|idea|vscode)/'
  '^cmake-build-debug/'
  '\.zip$'
  '\.tar\.gz$'
  '\.tgz$'
)

# Combine patterns into a single regex for grep/perl
GREP_EXCLUDE_REGEX=$(IFS=\|; echo "${EXCLUDE_PATTERNS[*]}")

echo "Collecting files to archive..."

# Create a temporary file to hold the selected file list (newline delimited).
TMP_LIST="$(mktemp --suffix=.files 2>/dev/null || mktemp)"
trap 'rm -f "$TMP_LIST"' EXIT

# Build the file list with the specific third_party rule:
# - include third_party/CMakeLists.txt
# - include third_party/cmake/**
# - exclude everything else under third_party/
# - otherwise apply EXCLUDE_PATTERNS globally
#
# We use newline-delimited git ls-files; git does not produce newline characters inside filenames.
git ls-files > "${TMP_LIST}.all"

# Create the filtered list
: > "$TMP_LIST"
while IFS= read -r f || [[ -n "$f" ]]; do
    # skip empty lines just in case
    [[ -z "$f" ]] && continue

    if [[ "$f" == third_party/* ]]; then
        # allow only exact third_party/CMakeLists.txt or entries under third_party/cmake/
        if [[ "$f" == "third_party/CMakeLists.txt" ]] || [[ "$f" == third_party/cmake/* ]]; then
            # still respect global exclude patterns (e.g. archive files)
            if ! printf '%s\n' "$f" | grep -Eq "$GREP_EXCLUDE_REGEX"; then
                printf '%s\n' "$f" >> "$TMP_LIST"
            fi
        else
            # explicitly skip other third_party entries
            continue
        fi
    else
        # not under third_party: apply normal exclusions
        if ! printf '%s\n' "$f" | grep -Eq "$GREP_EXCLUDE_REGEX"; then
            printf '%s\n' "$f" >> "$TMP_LIST"
        fi
    fi
done < "${TMP_LIST}.all"

# Count selected files
file_count=$(wc -l < "$TMP_LIST" | awk '{print $1}')
if [[ -z "$file_count" ]] || [[ "$file_count" -eq 0 ]]; then
    echo "No files found to archive after applying exclusions. Aborting."
    rm -f "${TMP_LIST}.all" "$TMP_LIST"
    exit 1
fi

echo "Found $file_count files. Creating archive..."


# --- Helper: detect tar transform support ---
tar_supports_transform() {
    # Check GNU tar (--transform) or BSD tar (-s) support.
    if tar --version >/dev/null 2>&1; then
        if tar --version 2>&1 | grep -q 'GNU'; then
            echo "gnu"
            return 0
        fi
    fi
    # check help for -s (bsd-style) or --transform
    if tar --help 2>&1 | grep -q -- '--transform'; then
        echo "transform"
        return 0
    fi
    if tar --help 2>&1 | grep -q -- '-s,'; then
        echo "bsd"
        return 0
    fi
    # unknown / no transform support
    echo "none"
    return 1
}

TAR_KIND="$(tar_supports_transform)"

# Create archive using transform if available; otherwise use staging dir.
if [[ "$TAR_KIND" = "gnu" || "$TAR_KIND" = "transform" ]]; then
    echo "Using tar --transform to prefix files with '${ARCHIVE_ROOT_DIR}/'"
    # Use --files-from with the newline-delimited list
    # Note: --null/--files-from - would be ideal for null-delimited, but we wrote newline list for portability.
    tar -czvf "$OUTPUT_FILE" --files-from="$TMP_LIST" --transform="s,^,${ARCHIVE_ROOT_DIR}/,"
    status=$?
elif [[ "$TAR_KIND" = "bsd" ]]; then
    echo "Using BSD tar -s to prefix files with '${ARCHIVE_ROOT_DIR}/'"
    # BSD tar uses -s; the substitution syntax is slightly different
    # We tell tar to read file list and apply -s to each path
    tar -czvf "$OUTPUT_FILE" --files-from="$TMP_LIST" -s ",^,${ARCHIVE_ROOT_DIR}/,"
    status=$?
else
    echo "tar does not support --transform or -s on this system. Using temporary staging directory."
    STAGING_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t pack_source)"
    trap 'rm -rf "$STAGING_DIR"; rm -f "${TMP_LIST}.all" "$TMP_LIST"' EXIT

    mkdir -p "${STAGING_DIR}/${ARCHIVE_ROOT_DIR}"

    # Copy each selected file into staging, preserving directory structure
    while IFS= read -r f || [[ -n "$f" ]]; do
        src="${SOURCE_DIR%/}/$f"
        dest="${STAGING_DIR}/${ARCHIVE_ROOT_DIR}/$f"
        destdir=$(dirname "$dest")
        mkdir -p "$destdir"
        cp -p -- "$src" "$dest"
    done < "$TMP_LIST"

    (cd "$STAGING_DIR" && tar -czvf "$OUTPUT_FILE" "$ARCHIVE_ROOT_DIR")
    status=$?
    # clean-up done by trap
fi

rm -f "${TMP_LIST}.all" "$TMP_LIST"

if [[ $status -eq 0 ]]; then
  echo "--------------------------------------------------"
  echo "Archive created successfully:"
  echo "  => $OUTPUT_FILE"
  echo "--------------------------------------------------"
  exit 0
else
  echo "--------------------------------------------------" >&2
  echo "Error: Archive creation failed (tar exit code: $status)." >&2
  echo "--------------------------------------------------" >&2
  exit $status
fi
