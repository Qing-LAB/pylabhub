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
  snapshot_cache_var(WITH_LIBSODIUM_STATIC)

  # --- 2. Set options for the isolated build scope ---
  snapshot_cache_var(POLLER)
  snapshot_cache_var(ZMQ_HAVE_IPC)
  if(MSVC)
    # libzmq's wepoll (epoll emulation via IOCP) has a teardown bug:
    # epoll_ctl(EPOLL_CTL_DEL) returns EINVAL during zmq_ctx_term() when
    # the socket fd is already closed, hitting errno_assert → abort().
    # libzmq's own poll.hpp recommends using select on Windows instead.
    # IPC transport requires epoll, but we only use TCP — disable IPC.
    set(POLLER "select" CACHE STRING "pylab: avoid wepoll teardown crash" FORCE)
    set(ZMQ_HAVE_IPC OFF CACHE BOOL "pylab: disable IPC (not used, incompatible with select poller)" FORCE)
  endif()
  set(BUILD_STATIC ON CACHE BOOL "pylab: build static libzmq" FORCE)
  set(BUILD_SHARED ON CACHE BOOL "pylab: build shared libzmq as a workaround" FORCE)
  set(ZMQ_BUILD_TESTS OFF CACHE BOOL "pylab: disable libzmq tests" FORCE)
  set(WITH_PERF_TOOL OFF CACHE BOOL "pylab: disable libzmq perf tools" FORCE)
  set(WITH_DOCS OFF CACHE BOOL "pylab: disable libzmq docs" FORCE)
  set(ENABLE_CURVE ON CACHE BOOL "pylab: required for security" FORCE)
  set(WITH_LIBSODIUM ON CACHE BOOL "pylab: required for CurveZMQ" FORCE)
  set(WITH_LIBSODIUM_STATIC ON CACHE BOOL "pylab: required for CurveZMQ" FORCE)
  
  # --- 3. Pre-populate Sodium variables for libzmq ---
  # libzmq's add_subdirectory calls find_package(sodium) at configure time, but our
  # libsodium is built via ExternalProject_Add at build time. On a fresh build the
  # prereqs dir is empty, so find_package(sodium) would fail.
  #
  # libzmq's Findsodium.cmake uses find_path(SODIUM_INCLUDE_DIRS ...) and
  # find_library(SODIUM_LIBRARIES ...). These skip their search if the result
  # variable is already set in the CACHE. We pre-populate the cache to point at
  # the prereq install directory where libsodium-stable will appear after the
  # ExternalProject builds. Build ordering is guaranteed by
  # add_dependencies(libzmq-static libsodium_external).
  set(SODIUM_INCLUDE_DIRS "${PREREQ_INSTALL_DIR}/include" CACHE PATH "pylabhub: prereq libsodium headers" FORCE)
  set(SODIUM_LIBRARIES "${PREREQ_INSTALL_DIR}/lib/libsodium-stable${CMAKE_STATIC_LIBRARY_SUFFIX}" CACHE FILEPATH "pylabhub: prereq libsodium library" FORCE)
  set(SODIUM_LIBRARY_DIRS "${PREREQ_INSTALL_DIR}/lib")

  # --- 4. Add the subdirectory ---
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libzmq EXCLUDE_FROM_ALL)

  # --- 5. Create the explicit dependency ---
  # libzmq creates an OBJECT library ("objects") whose .o files are compiled
  # independently and then linked into both libzmq (shared) and libzmq-static.
  # add_dependencies on libzmq-static only gates the final link step — it does
  # NOT prevent the object compilation from starting before libsodium headers
  # are installed. We must add the dependency to every target that compiles
  # libzmq source files.
  add_dependencies(libzmq-static libsodium_external)
  if(TARGET objects)
    add_dependencies(objects libsodium_external)
  endif()
  if(TARGET libzmq)
    add_dependencies(libzmq libsodium_external)
  endif()

  # --- 6. Create canonical targets ---
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
    # STABLE_NAME normalizes libzmq's versioned output (e.g., libzmq-vc142-mt-s-4_3_6.lib
    # on Windows) to libzmq-stable.a / libzmq-stable.lib for cross-platform consistency.
    pylabhub_register_library_for_staging(TARGET libzmq-static STABLE_NAME libzmq-stable)
  endif()

  # --- 8. Restore cache variables ---
  restore_cache_var(BUILD_STATIC BOOL)
  restore_cache_var(BUILD_SHARED BOOL)
  restore_cache_var(ZMQ_BUILD_TESTS BOOL)
  restore_cache_var(WITH_PERF_TOOL BOOL)
  restore_cache_var(WITH_DOCS BOOL)
  restore_cache_var(ENABLE_CURVE BOOL)
  restore_cache_var(WITH_LIBSODIUM BOOL)
  restore_cache_var(WITH_LIBSODIUM_STATIC BOOL)
  restore_cache_var(POLLER STRING)
  restore_cache_var(ZMQ_HAVE_IPC BOOL)

  message(STATUS "[pylabhub-third-party] libzmq configuration complete.")
else()
  message(FATAL_ERROR "[pylabhub-third-party] libzmq submodule not found.")
endif()
