# Platform TODO

**Scope:** Windows / MSVC / cross-platform / CMake / packaging.
**Strategic status:** `docs/TODO_MASTER.md`.  This file holds platform-
specific detail for open items only.

---

## Current Focus

### Platform support claims vs CI validation (2026-03-15)

- [ ] **CI is Linux-only** but `pyproject.toml` classifiers advertise
  macOS and Windows, and `docs/README/README_testing.md` says all tests
  must run on Windows/Linux/macOS/FreeBSD.
  `docs/README/README_utils.md` still says DataBlock-on-Windows is
  incomplete.  Either add macOS/Windows CI jobs or narrow the
  documented platform support to match what is actually validated.

### N6 (#86) — User-oriented `cmake/pylabhubNativePlugin.cmake` helper

**Drop-in CMake module for native-plugin authors** who use pylabhub
as a dependency.  Must NOT be project-internal CMake — this is a
deliverable for downstream plugin authors.

Plugin author drops the module into their own project, calls:

```cmake
find_package(pylabhubNativePlugin REQUIRED
             PATHS /path/to/pylabhub/install)
plh_add_native_plugin(my_plugin SOURCES my_plugin.cpp)
```

Helper deduces include + lib paths from the install location;
provides the `plh_add_native_plugin(target SOURCES files...)` macro
that handles `-shared -fPIC -std=c++20 -I<include>` plus correct
link visibility (header-only `native_engine_api.h` — no link to
`pylabhub::utils` for plugins using the C API; optional link for
the `plh::Context` C++ wrapper).

Pairs with `docs/README/README_NativePlugins.md` (task #86).

---

## Backlog

### Clang-tidy quality pass

- [ ] Reconfigure with `CC=clang CXX=clang++` and
  `-DPYLABHUB_ENABLE_CLANG_TIDY=ON` for a complete static-analysis
  sweep.  GCC build is clean; clang-tidy adds cppcoreguidelines,
  modernize, etc.  Infra wired in `cmake/PlatformAndCompiler.cmake`.
  Run periodically.

```bash
CC=clang CXX=clang++ cmake -S . -B build-clang \
    -DPYLABHUB_ENABLE_CLANG_TIDY=ON
cmake --build build-clang 2>&1 | grep -E "warning:|error:" | \
    grep -v third_party
```

### Windows (MSVC) — known gaps

- [ ] **`/Zc:preprocessor` PUBLIC propagation audit.**  Confirm all
  consumers of `pylabhub::utils` that use `PLH_DEBUG` / `LOGGER_*`
  macros receive the flag via CMake interface propagation.  Run a
  Windows CI build targeting at least one consumer executable and
  verify no C3878 / C2760 VA_OPT errors.  Site:
  `src/utils/CMakeLists.txt`.

- [ ] **MSVC warnings-as-errors gate.**  Add `/W4 /WX` to MSVC CI to
  catch future C4251 / C4324 / C4996 regressions early.  Currently
  only runs on Linux with `-Werror`.

---

## Notes

### MSVC vs GCC differences to remember
- MSVC requires `/Zc:preprocessor` for `__VA_OPT__` / variadic-macro
  semantics consistent with GCC + Clang.
- DLL boundary: types crossing PUBLIC API need explicit
  `PYLABHUB_UTILS_EXPORT` annotation (see HEP-CORE-0032).
- `_CRT_SECURE_NO_WARNINGS` not blanket-set — site-by-site `#pragma
  warning(suppress)` instead.

### Pattern: FlexZone / DataBlock POD requirement
DataBlock + FlexZone payloads must be POD (standard-layout +
trivially-copyable) for cross-platform memory layout to match.
Test: `std::is_standard_layout<T>::value && std::is_trivially_
copyable<T>::value`.  Enforced at the schema-validation layer.

---

## Related Work

- `docs/HEP/HEP-CORE-0032-ABI-Compatibility.md` — ABI policy for
  cross-toolchain builds.
- `docs/README/README_CMake_Design.md` — project build structure.
