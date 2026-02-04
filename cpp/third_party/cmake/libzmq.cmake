# third_party/cmake/libzmq.cmake
#
# This script is a wrapper for building the libzmq (ZeroMQ) library.
#
# libzmq is a well-behaved CMake project, so it is built as a native sub-project
# using `add_subdirectory`. This provides a more robust dependency graph than
# using ExternalProject_Add, especially for handling its dependency on libsodium.
#
# The strategy is to:
#  1. Isolate the build using `snapshot_cache_var` and `restore_cache_var`.
#  2. Set libzmq's internal options (`BUILD_STATIC`, etc.).
#  3. Call `add_subdirectory(libzmq)`. Its internal `find_package(Sodium)` will
#     succeed because we set `CMAKE_PREFIX_PATH` in the parent scope.
#  4. Manually create the dependency between the created `libzmq-static` target
#     and the `libsodium_external` target to ensure correct build order.
#  5. Register the `libzmq` artifacts for staging.

include(ThirdPartyPolicyAndHelper)
include(StageHelpers)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/CMakeLists.txt")
  message(STATUS "[pylabhub-third-party] Configuring libzmq submodule...")

  # --- 1. Snapshot cache variables to isolate the build ---
  snapshot_cache_var(BUILD_STATIC)
  snapshot_cache_var(BUILD_SHARED)
  snapshot_cache_var(ZMQ_BUILD_TESTS)
  snapshot_cache_var(WITH_PERF_TOOL)
  snapshot_cache_var(WITH_DOCS)
  snapshot_cache_var(ENABLE_CURVE)
  snapshot_cache_var(WITH_LIBSODIUM)

  # --- 2. Set options for the isolated build scope ---
  set(BUILD_STATIC ON CACHE BOOL "pylab: build static libzmq" FORCE)
  set(BUILD_SHARED ON CACHE BOOL "pylab: build shared libzmq as a workaround" FORCE)
  set(ZMQ_BUILD_TESTS OFF CACHE BOOL "pylab: disable libzmq tests" FORCE)
  set(WITH_PERF_TOOL OFF CACHE BOOL "pylab: disable libzmq perf tools" FORCE)
  set(WITH_DOCS OFF CACHE BOOL "pylab: disable libzmq docs" FORCE)
  set(ENABLE_CURVE ON CACHE BOOL "pylab: required for security" FORCE)
  set(WITH_LIBSODIUM ON CACHE BOOL "pylab: required for CurveZMQ" FORCE)
  
  # --- 3. Add the subdirectory ---
  # `find_package(Sodium)` inside this call will now search the `CMAKE_PREFIX_PATH`
  # which was set in the parent CMakeLists.txt to point to our `prereqs` dir.
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libzmq EXCLUDE_FROM_ALL)

  # --- 4. Create the explicit dependency ---
  # This is the crucial step to fix the build order. It tells make that
  # libzmq-static cannot be built until libsodium_external is finished.
  add_dependencies(libzmq-static libsodium_external)

  # --- 5. Create canonical targets ---
  # Create the canonical INTERFACE wrapper target.
  add_library(pylabhub_libzmq INTERFACE)
  # Link the wrapper to the concrete implementation target provided by add_subdirectory.
  target_link_libraries(pylabhub_libzmq INTERFACE libzmq-static)
  # Create the final, public-facing ALIAS.
  add_library(pylabhub::third_party::libzmq ALIAS pylabhub_libzmq)

  if(THIRD_PARTY_INSTALL)
    pylabhub_register_headers_for_staging(
      DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/include"
      SUBDIR ""
    )
    pylabhub_register_library_for_staging(TARGET libzmq-static)
  endif()

  # --- 7. Restore cache variables ---
  restore_cache_var(BUILD_STATIC BOOL)
  restore_cache_var(BUILD_SHARED BOOL)
  restore_cache_var(ZMQ_BUILD_TESTS BOOL)
  restore_cache_var(WITH_PERF_TOOL BOOL)
  restore_cache_var(WITH_DOCS BOOL)
  restore_cache_var(ENABLE_CURVE BOOL)
  restore_cache_var(WITH_LIBSODIUM BOOL)

  message(STATUS "[pylabhub-third-party] libzmq configuration complete.")
else()
  message(FATAL_ERROR "[pylabhub-third-party] libzmq submodule not found.")
endif()
