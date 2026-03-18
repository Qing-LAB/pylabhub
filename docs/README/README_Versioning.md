# pyLabHub Versioning

This document describes the pyLabHub version system: what each version tracks,
where the source of truth lives, and how to update each one.

## Table of Contents

1. [Version Domains](#1-version-domains)
2. [Single Source of Truth](#2-single-source-of-truth)
3. [Data Flow](#3-data-flow)
4. [Update Procedures](#4-update-procedures)
5. [Querying Versions](#5-querying-versions)
6. [Library Output Naming](#6-library-output-naming)
7. [Overriding the Version](#7-overriding-the-version)
8. [ABI Compatibility Rules](#8-abi-compatibility-rules)

---

## 1. Version Domains

pyLabHub has three independent version domains:

| Domain | Example | Cadence | Source of truth |
|--------|---------|---------|-----------------|
| **Release** | `0.1.0a0` | Per PyPI release / git tag | `cmake/Versions.cmake` |
| **Python runtime** | `3.14.3+20260211` | When upgrading bundled Python | `cmake/Versions.cmake` |
| **ABI / protocol** | shm=1.0, wire=1.0, script=1.0 | When internal layouts change | `version_registry.cpp` |

### Release version

The user-visible package version, following [PEP 440](https://peps.python.org/pep-0440/).
Used for PyPI uploads, git tags, and `pylabhub.__version__`.

### Python runtime version

The astral-sh/python-build-standalone release pinned for the embedded Python
interpreter. Used at build time (CMake downloads headers/libs for pybind11) and
at install time (`pylabhub prepare-runtime` downloads the runtime).

### ABI / protocol versions

Internal compatibility markers in `src/utils/core/version_registry.cpp`:

| Component | What it tracks | Bump major when | Bump minor when |
|-----------|---------------|-----------------|-----------------|
| `shm` | `SharedMemoryHeader` layout | Field removed/reordered | New field appended |
| `wire` | Control-plane protocol (REG_REQ, DISC_ACK) | Field removed/renamed | New optional field |
| `script_api` | Python/Lua callback signatures + API | Method removed/renamed | New method added |
| `facade` | `sizeof(ProducerMessagingFacade)` etc. | N/A (ABI canary) | N/A |

### Library SOVERSION

Derived from the release version. Format: `major.minor.rolling` where rolling
is the git commit count (`git rev-list --count HEAD`). Used for shared library
VERSION/SOVERSION properties on POSIX.

---

## 2. Single Source of Truth

**`cmake/Versions.cmake`** — defines all release and runtime version constants:

```cmake
set(PYLABHUB_RELEASE_VERSION        "0.1.0a0")
set(PYLABHUB_PYTHON_RUNTIME_VERSION "3.14.3+20260211")
set(PYLABHUB_PYTHON_RELEASE_TAG     "20260211")
```

**`pyproject.toml`** — must match `PYLABHUB_RELEASE_VERSION` manually (marked
with a sync comment). scikit-build-core reads it for wheel metadata.

**`src/utils/core/version_registry.cpp`** — ABI version constants:

```cpp
static constexpr uint8_t kShmMajor = 1;
static constexpr uint8_t kWireMajor = 1;
static constexpr uint8_t kScriptApiMajor = 1;
```

---

## 3. Data Flow

```
cmake/Versions.cmake  (single source of truth for release + python runtime)
  |
  +---> CMakeLists.txt project(VERSION ...)
  |
  +---> pylabhub_version.h.in  --configure_file-->  pylabhub_version.h
  |       C++ macros: PYLABHUB_VERSION_*, PYLABHUB_RELEASE_VERSION,
  |       PYLABHUB_PYTHON_RUNTIME_VERSION
  |
  +---> pylabhub_version.py.in --configure_file-->  python/pylabhub/_version_generated.py
  |       __version__, PYTHON_RUNTIME_VERSION, PYTHON_RELEASE_TAG
  |
  +---> third_party/cmake/python.cmake
  |       Build-time Python download URL
  |
  +---> python/pylabhub/_runtime.py
          Post-install Python download (imports _version_generated)

version_registry.cpp  (ABI versions, compiled into shared lib)
  |
  +---> version_info_json() / version_info_string()   (C++ API)
  +---> pylabhub_abi_info_json()                       (extern "C", ABI-stable)
  +---> api.version_info()                             (Lua/Python script API)
```

---

## 4. Update Procedures

### Bumping the release version

1. Edit `cmake/Versions.cmake`: set `PYLABHUB_RELEASE_VERSION`
2. Edit `pyproject.toml`: update `version = "..."` to match
3. Run `cmake -S . -B build` to regenerate `_version_generated.py`
4. Commit, tag: `git tag v<version>`

### Upgrading the Python runtime

1. Edit `cmake/Versions.cmake`: set `PYLABHUB_PYTHON_RUNTIME_VERSION` and `PYLABHUB_PYTHON_RELEASE_TAG`
2. Update SHA-256 hashes in `python/pylabhub/_runtime.py` `PYTHON_BUILDS` table
3. Run `cmake -S . -B build` to regenerate headers
4. Rebuild and test — pybind11 ABI depends on these headers

**Impact:** All pybind11 bindings are compiled against these headers.
A mismatch between build-time and runtime Python will cause crashes.

### Bumping an ABI version

1. Edit `src/utils/core/version_registry.cpp`: bump `kXxxMajor`/`kXxxMinor`
2. Update `docs/HEP/HEP-CORE-0026-Version-Registry.md` if semantics change
3. Rebuild — new values propagate through `version_info_json()`

**Impact:** Major mismatch should be rejected at boundaries (e.g., consumer
built against shm=2.0 cannot read shm=1.0 segments). Minor mismatches are
additive (log warning, proceed).

---

## 5. Querying Versions

### From C++

```cpp
#include "plh_version_registry.hpp"

auto v = pylabhub::version::current();         // ComponentVersions struct
auto s = pylabhub::version::version_info_string(); // human-readable one-liner
auto j = pylabhub::version::version_info_json();   // JSON string
const char *rel = pylabhub::version::release_version();
const char *py  = pylabhub::version::python_runtime_version();
```

### From Python scripts (embedded in role callback)

```python
info = api.version_info()  # JSON string
```

### From Lua scripts (embedded in role callback)

```lua
local info = api.version_info()  -- JSON string
```

### From host Python (`pip install pylabhub`)

```python
import pylabhub
pylabhub.__version__               # "0.1.0a0"
pylabhub.python_runtime_version    # "3.14.3+20260211"
pylabhub.abi_versions()            # dict with all ABI fields (via ctypes)
```

### From C / FFI / dlsym

```c
/* extern "C" — ABI-stable, returns const char* to static buffer */
const char *json = pylabhub_abi_info_json();
```

---

## 6. Library Output Naming

The pylabhub-utils shared library uses version suffixes:

| Platform | Output | Version metadata |
|----------|--------|------------------|
| **Linux** | `libpylabhub-utils-stable.so.0.1.42` + soname symlink | Filename |
| **macOS** | `libpylabhub-utils-stable.0.1.42.dylib` + soname symlink | Mach-O load commands |
| **Windows** | `pylabhub-utils-stable.dll` | PE VERSIONINFO resource |

On Windows, right-click the DLL > Properties > Details to view version info.

---

## 7. Overriding the Version

### Rolling version

Override when git is unavailable or for reproducible builds:

```bash
cmake -S . -B build -DPYLABHUB_VERSION_ROLLING=123
```

### Release version

Edit `cmake/Versions.cmake` and `pyproject.toml` (see §4).

---

## 8. ABI Compatibility Rules

For **shared memory structures** (`SharedMemoryHeader`, DataBlock layout):

- **Named constants:** Use `detail::MAX_CONSUMER_HEARTBEATS` etc. for array sizes.
  No hardcoded magic numbers in structure definitions.
- **Version association:** Layout constants live in a `detail` namespace with
  version (`HEADER_VERSION_MAJOR/MINOR`). Changing pool sizes or field order is
  ABI-breaking; bump major version and document migration.
- **Static assertions:** `static_assert(sizeof(SharedMemoryHeader) == 4096, ...)`
  for critical sizes so layout drift is caught at compile time.
- **Size calculation comments:** Document how reserved/padding bytes are computed.

For **exported functions** (`PYLABHUB_UTILS_EXPORT`):

- All public classes use pimpl (no public data members of implementation types).
- C-linkage functions (`extern "C"`) use POD types only.
- The `pylabhub_abi_info_json()` function is the stable entry point for
  external consumers (ctypes, dlsym, FFI).

See `src/include/utils/data_block.hpp` for canonical layout constants and assertions.

---

## See Also

- [README_CMake_Design.md](README_CMake_Design.md) — Build system architecture
- [README_utils.md](README_utils.md) — Utils library overview
- [HEP-CORE-0026](../HEP/HEP-CORE-0026-Version-Registry.md) — Version registry design
