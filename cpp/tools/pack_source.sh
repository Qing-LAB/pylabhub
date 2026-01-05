#!/usr/bin/env bash
# tools/pack_source.sh (portable / macOS-friendly)
# Archives project source files into a gzipped tarball.
# this script was made to work with both gnu and bsd style 
# tar and other commands so it should run under linux and mac/freebsd
# -------------------------------------------------------------------
set -euo pipefail
IFS=$'\n\t'

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="${PROJECT_ROOT}"
PROJECT_NAME=$(basename "$PROJECT_ROOT")
OUTPUT_FILE="${PROJECT_ROOT}/${PROJECT_NAME}-src.tar.gz"

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

while [[ $# -gt 0 ]]; do
  case "$1" in
    -o|--output)
      if [[ -z "${2-}" ]]; then echo "Missing argument for $1" >&2; usage; exit 1; fi
      if [[ "$2" != /* ]]; then OUTPUT_FILE="$(pwd)/$2"; else OUTPUT_FILE="$2"; fi
      shift 2
      ;;
    -d|--dir)
      if [[ -z "${2-}" ]]; then echo "Missing argument for $1" >&2; usage; exit 1; fi
      SOURCE_DIR="$2"; shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

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

ARCHIVE_ROOT_DIR="${PROJECT_NAME}-src"

echo "--------------------------------------------------"
echo "Project root:    $SOURCE_DIR"
echo "Output archive:  $OUTPUT_FILE"
echo "Archive root dir: $ARCHIVE_ROOT_DIR"
echo "--------------------------------------------------"

EXCLUDE_PATTERNS=(
  '^build/'
  '^\.(git|idea|vscode)/'
  '^cmake-build-debug/'
  '\.zip$'
  '\.tar\.gz$'
  '\.tgz$'
)
GREP_EXCLUDE_REGEX=$(IFS=\|; echo "${EXCLUDE_PATTERNS[*]}")

echo "Collecting files to archive..."

# portable mktemp usage
TMP_ALL="$(mktemp 2>/dev/null || mktemp -t pack_source)"
TMP_LIST="$(mktemp 2>/dev/null || mktemp -t pack_source.list)"
trap 'rm -f "$TMP_ALL" "$TMP_LIST"' EXIT

git ls-files > "$TMP_ALL"

: > "$TMP_LIST"
while IFS= read -r f || [[ -n "$f" ]]; do
    [[ -z "$f" ]] && continue
    if [[ "$f" == third_party/* ]]; then
        if [[ "$f" == "third_party/CMakeLists.txt" ]] || [[ "$f" == third_party/cmake/* ]]; then
            if ! printf '%s\n' "$f" | grep -Eq "$GREP_EXCLUDE_REGEX"; then
                printf '%s\n' "$f" >> "$TMP_LIST"
            fi
        else
            continue
        fi
    else
        if ! printf '%s\n' "$f" | grep -Eq "$GREP_EXCLUDE_REGEX"; then
            printf '%s\n' "$f" >> "$TMP_LIST"
        fi
    fi
done < "$TMP_ALL"

file_count=$(wc -l < "$TMP_LIST" | awk '{print $1}')
if [[ -z "$file_count" ]] || [[ "$file_count" -eq 0 ]]; then
    echo "No files found to archive after applying exclusions. Aborting."
    exit 1
fi

echo "Found $file_count files. Creating archive..."

# --- Improved tar feature detection ---
tar_supports_transform() {
    # Prefer explicit version query
    if tar --version >/dev/null 2>&1; then
        ver="$(tar --version 2>&1 | tr '[:upper:]' '[:lower:]' | head -n1 || true)"
        if echo "$ver" | grep -q 'gnu'; then
            echo "gnu"; return 0
        fi
        if echo "$ver" | grep -q 'bsdtar'; then
            echo "bsd"; return 0
        fi
    fi
    # Fall back to checking help output for --transform or -s
    if tar --help 2>&1 | grep -q -- '--transform'; then
        echo "transform"; return 0
    fi
    if tar --help 2>&1 | grep -q -- '\-s'; then
        echo "bsd"; return 0
    fi
    echo "none"; return 1
}

TAR_KIND="$(tar_supports_transform)"

if [[ "$TAR_KIND" = "gnu" || "$TAR_KIND" = "transform" ]]; then
    echo "Using tar --transform to prefix files with '${ARCHIVE_ROOT_DIR}/'"
    tar -czvf "$OUTPUT_FILE" --files-from="$TMP_LIST" --transform="s,^,${ARCHIVE_ROOT_DIR}/," || status=$?
    status=${status:-$?}
elif [[ "$TAR_KIND" = "bsd" ]]; then
    echo "Using BSD tar -s to prefix files with '${ARCHIVE_ROOT_DIR}/'"
    # BSD tar expects -s pattern in single argument form; use a portable quoting
    tar -czvf "$OUTPUT_FILE" --files-from="$TMP_LIST" -s ",^,${ARCHIVE_ROOT_DIR}/," || status=$?
    status=${status:-$?}
else
    echo "tar does not support --transform or -s on this system. Using temporary staging directory."
    STAGING_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t pack_source)"
    trap 'rm -rf "$STAGING_DIR"; rm -f "$TMP_ALL" "$TMP_LIST"' EXIT

    mkdir -p "${STAGING_DIR}/${ARCHIVE_ROOT_DIR}"

    while IFS= read -r f || [[ -n "$f" ]]; do
        src="${SOURCE_DIR%/}/$f"
        dest="${STAGING_DIR}/${ARCHIVE_ROOT_DIR}/$f"
        destdir=$(dirname "$dest")
        mkdir -p "$destdir"
        # remove GNU-only "--" to be compatible with BSD cp
        cp -p "$src" "$dest"
    done < "$TMP_LIST"

    # create archive inside staging dir so path prefixes are correct
    (cd "$STAGING_DIR" && tar -czvf "$OUTPUT_FILE" "$ARCHIVE_ROOT_DIR")
    status=$?
fi

rm -f "$TMP_ALL" "$TMP_LIST"

if [[ ${status:-0} -eq 0 ]]; then
  echo "--------------------------------------------------"
  echo "Archive created successfully:"
  echo "  => $OUTPUT_FILE"
  echo "--------------------------------------------------"
  exit 0
else
  echo "--------------------------------------------------" >&2
  echo "Error: Archive creation failed (tar exit code: ${status:-1})." >&2
  echo "--------------------------------------------------" >&2
  exit ${status:-1}
fi
