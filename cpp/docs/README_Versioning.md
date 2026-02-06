# pyLabHub Versioning

This document describes the versioning scheme used by the pyLabHub C++ project, how version information is determined and propagated, and how to access it from both CMake and C++.

## Table of Contents

1. [Version Scheme](#1-version-scheme)
2. [Version Sources](#2-version-sources)
3. [CMake Integration](#3-cmake-integration)
4. [C++ API](#4-c-api)
5. [Library Output Naming](#5-library-output-naming)
6. [Overriding the Version](#6-overriding-the-version)

---

## 1. Version Scheme

The pyLabHub package uses a three-part version string:

```
major.minor.rolling
```

| Component | Source | Description |
|-----------|--------|-------------|
| **major** | `project()` VERSION | Breaking API changes; incremented for incompatible changes. |
| **minor** | `project()` VERSION | New features; incremented for backward-compatible additions. |
| **rolling** | Git or override | Development build identifier; typically `git rev-list --count HEAD`. |

**Examples:**

- `0.1.42` — major 0, minor 1, rolling 42
- `1.0.1234` — major 1, minor 0, rolling 1234

---

## 2. Version Sources

- **major** and **minor** come from the top-level `project()` call in `CMakeLists.txt`:

  ```cmake
  project(pylabhub-cpp VERSION 0.1 LANGUAGES C CXX)
  ```

- **rolling** is obtained at configure time by running:

  ```bash
  git rev-list --count HEAD
  ```

  If the project is not a git repository or git is unavailable, rolling defaults to `0`. You can override it with `-DPYLABHUB_VERSION_ROLLING=N` (see [§6](#6-overriding-the-version)).

---

## 3. CMake Integration

Version handling is centralized in `cmake/Version.cmake`, which is included after `project()` in the top-level `CMakeLists.txt`.

**Variables set by Version.cmake:**

| Variable | Description | Example |
|----------|-------------|---------|
| `PYLABHUB_VERSION_MAJOR` | Major version | `0` |
| `PYLABHUB_VERSION_MINOR` | Minor version | `1` |
| `PYLABHUB_VERSION_ROLLING` | Rolling version | `42` |
| `PYLABHUB_VERSION_STRING` | Full version string | `"0.1.42"` |

These variables are used for:

- Shared library `VERSION` and `SOVERSION` (e.g., `libpylabhub-utils-stable.so.0.1.42`)
- Generating `pylabhub_version.h` for C++ access
- Configure-time status messages

---

## 4. C++ API

Version information is exposed via the `pylabhub::platform` namespace. Include `plh_platform.hpp`:

```cpp
#include "plh_platform.hpp"

// Individual components
int major  = pylabhub::platform::get_version_major();    // 0
int minor  = pylabhub::platform::get_version_minor();    // 1
int rolling = pylabhub::platform::get_version_rolling(); // 42

// Full string
const char* ver = pylabhub::platform::get_version_string(); // "0.1.42"
```

**Generated header:** The build system generates `pylabhub_version.h` in the build tree from `cmake/pylabhub_version.h.in`. It defines:

- `PYLABHUB_VERSION_MAJOR`
- `PYLABHUB_VERSION_MINOR`
- `PYLABHUB_VERSION_ROLLING`
- `PYLABHUB_VERSION_STRING`

The C++ functions return these values. The header is installed with the library for downstream consumers.

---

## 5. Library Output Naming

The pylabhub-utils shared library uses a stable base name with version suffixes:

| Platform | Build Output | Staged Output | Version Metadata |
|----------|--------------|---------------|------------------|
| **Linux** | `libpylabhub-utils-stable.so.0.1.42` | Same + soname symlink | Filename only |
| **macOS** | `libpylabhub-utils-stable.0.1.42.dylib` | Same + soname symlink | Mach-O load commands |
| **Windows** | `pylabhub-utils-stable.dll` | `pylabhub-utils-stable.dll` | PE VERSIONINFO resource |

On Windows, version is embedded via a VERSIONINFO resource (generated from `cmake/pylabhub_utils_version.rc.in`). Right-click the DLL → Properties → Details to view File Version and Product Version.

---

## 6. Overriding the Version

### CMake

Override the rolling version when git is unavailable or for reproducible builds:

```bash
cmake -S cpp -B cpp/build -DPYLABHUB_VERSION_ROLLING=123
```

### Major/Minor

To change major and minor, edit the top-level `project()` call in `cpp/CMakeLists.txt`:

```cmake
project(pylabhub-cpp VERSION 1.2 LANGUAGES C CXX)
```

---

## See Also

- [README_CMake_Design.md](README_CMake_Design.md) — Build system architecture
- [README_utils.md](README_utils.md) — Utils library overview
- [README_testing.md](README_testing.md) — Test suite and how to run version tests
