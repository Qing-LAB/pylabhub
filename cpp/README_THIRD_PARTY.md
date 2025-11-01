# Third-party integration (third_party)

This document describes how to add and configure third-party modules (submodules)
for the `cpp/` portion of the `pylabhub` repository.

## Guiding principle

Do **not** automatically `add_subdirectory()` for every folder under `third_party/`.
Different third-party projects often require per-project tuning (cache variables,
dependency wiring, toggling tests/perf, special compile/link flags). For that reason,
we prefer **explicit per-submodule blocks** in `cpp/CMakeLists.txt`.

Each submodule should get its own block that:

1. Tests for the presence of the submodule (e.g. `if(EXISTS "${THIRD_PARTY_DIR}/libname/CMakeLists.txt")`).
2. Sets the appropriate CMake cache variables BEFORE calling `add_subdirectory()` so the subproject reads them.
3. Calls `add_subdirectory()` into a dedicated build folder under `${CMAKE_CURRENT_BINARY_DIR}/third_party/<name>`.
4. Records any exported CMake target name(s) into a variable (for example `ZMQ_LIB_TARGET`) for later linking.

## Example: libzmq

Below is the pattern used in this repo for libzmq and can be used as a template for other submodules:

```cmake
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

if(EXISTS "${THIRD_PARTY_DIR}/libzmq/CMakeLists.txt")
  # Option to control shared/static from top-level configure:
  option(LIBZMQ_BUILD_SHARED "Build libzmq as shared library" OFF)
  if(LIBZMQ_BUILD_SHARED)
    set(LIBZMQ_SHARED_OPT ON CACHE BOOL "Build libzmq shared" FORCE)
  else()
    set(LIBZMQ_SHARED_OPT OFF CACHE BOOL "Build libzmq static" FORCE)
  endif()

  # Disable long-running tests/perf builds for CI
  set(BUILD_TESTS OFF CACHE BOOL "Disable libzmq tests" FORCE)
  set(BUILD_PERF OFF CACHE BOOL "Disable libzmq perf" FORCE)
  set(ENABLE_CURVE OFF CACHE BOOL "Disable libzmq CURVE by default" FORCE)

  add_subdirectory("${THIRD_PARTY_DIR}/libzmq" "${CMAKE_CURRENT_BINARY_DIR}/third_party/libzmq_build")

  # Capture the produced CMake target name if the subproject exports one
  if(TARGET zmq::libzmq)
    set(ZMQ_LIB_TARGET "zmq::libzmq")
  elseif(TARGET libzmq)
    set(ZMQ_LIB_TARGET "libzmq")
  endif()
endif()

```
