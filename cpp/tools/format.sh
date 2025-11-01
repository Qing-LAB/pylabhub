#!/usr/bin/env bash
set -euo pipefail

# locate project root
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# clang-format all C/C++/headers
echo "Running clang-format..."
find "${ROOT}" -name '*.cpp' -o -name '*.c' -o -name '*.hpp' -o -name '*.h' | \
  xargs -r clang-format -i

# cmake-format all CMakeLists and cmake files
echo "Running cmake-format..."
if command -v cmake-format >/dev/null 2>&1; then
  find "${ROOT}" -name 'CMakeLists.txt' -o -name '*.cmake' | \
    xargs -r cmake-format -i --config-file "${ROOT}/cmake-format.yaml"
else
  echo "cmake-format not installed; pip install cmake-format"
  exit 1
fi

echo "Formatting complete."

