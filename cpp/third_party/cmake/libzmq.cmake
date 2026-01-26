include(ThirdPartyPolicyAndHelper) # Ensure helpers are available.
# ---------------------------------------------------------------------------
# third_party/cmake/libzmq.cmake
# Wrapper for libzmq (ZeroMQ) third_party subproject
#
# Responsibilities:
#  - Configure the upstream libzmq subproject in an isolated scope using the
#    snapshot/restore cache helpers.
#  - Discover the canonical concrete `libzmq` library target.
#  - Provide a stable, namespaced `pylabhub::third_party::zmq` target for consumers.
#  - Stage the `libzmq` headers and library artifacts into the project's
#    `PYLABHUB_STAGING_DIR` by attaching custom commands to the
# - PYLABHUB_STAGING_DIR: (PATH)
#    `stage_third_party_deps` target.
# ---------------------------------------------------------------------------
#
# This script is controlled by the following global options/variables:
#
# - THIRD_PARTY_ZMQ_FORCE_VARIANT: (CACHE STRING: "static" | "shared" | "none")
#   Forces the build to be static or shared.
#
# - PYLABHUB_ZMQ_WITH_OPENPGM: (CACHE BOOL: ON | OFF)
#   Enables the OpenPGM reliable multicast transport.
#
# - PYLABHUB_ZMQ_WITH_NORM: (CACHE BOOL: ON | OFF)
#   Enables the NORM reliable multicast transport.
#
# - PYLABHUB_ZMQ_WITH_VMCI: (CACHE BOOL: ON | OFF)
#   Enables the VMCI transport for VM-to-VM communication.
#
# - THIRD_PARTY_DISABLE_TESTS: (CACHE BOOL: ON | OFF)
#   If ON, disables the build of libzmq's internal tests.
#
# - THIRD_PARTY_ALLOW_UPSTREAM_PCH: (CACHE BOOL: ON | OFF)
#   If OFF (default), disables libzmq's precompiled header options.
#
# - THIRD_PARTY_INSTALL: (CACHE BOOL: ON | OFF)
#   If ON, enables the post-build staging of libzmq artifacts.
#
# Internal Build Control & Isolation:
# This script uses `snapshot_cache_var` and `restore_cache_var` to create an
# isolated build environment. It translates high-level options into the specific
# CACHE variables that the libzmq build system understands (e.g., `BUILD_STATIC`,
# `ZMQ_BUILD_TESTS`). Any pre-existing global values for these variables are
# saved before `add_subdirectory(libzmq)` and restored immediately after.
#
# The following are provided by the top-level build environment:
#
# - THIRD_PARTY_STAGING_DIR: (PATH)
#   The absolute path to the staging directory where artifacts will be copied.
#
# - stage_third_party_deps: (CMake Target)
#   The custom target "hook" to which staging commands are attached.
# ---------------------------------------------------------------------------

# Include the top-level staging helpers. The path is added by the root CMakeLists.txt.
include(StageHelpers)


if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/CMakeLists.txt")
  message(STATUS "[pylabhub-third-party] Configuring libzmq submodule...")

  # ---------------------------
  # Snapshot cache vars we will touch so we can restore them later and avoid leakage.
  # (snapshot_cache_var/restore_cache_var should be provided by your cmake helpers.)
  # ---------------------------
  snapshot_cache_var(BUILD_SHARED)
  snapshot_cache_var(BUILD_STATIC)
  snapshot_cache_var(BUILD_SHARED_LIBS)
  snapshot_cache_var(ZMQ_BUILD_TESTS)
  snapshot_cache_var(BUILD_TESTS)
  snapshot_cache_var(ENABLE_PRECOMPILED)

  snapshot_cache_var(ZMQ_USE_PRECOMPILED_HEADER)
  snapshot_cache_var(ZMQ_USE_PCH)
  snapshot_cache_var(ENABLE_PRECOMPILED_HEADER)

  # Additional knobs used to minimize libzmq build footprint (examples/docs/tools)
  snapshot_cache_var(WITH_PERF_TOOL)
  snapshot_cache_var(WITH_DOCS)
  snapshot_cache_var(WITH_DOC)
  snapshot_cache_var(ZMQ_BUILD_FRAMEWORK)
  snapshot_cache_var(POLLER)
  snapshot_cache_var(WITH_OPENPGM)
  snapshot_cache_var(WITH_NORM)
  snapshot_cache_var(WITH_VMCI)
  snapshot_cache_var(ZMQ_BUILD_EXAMPLES)
  snapshot_cache_var(ZMQ_BUILD_TOOLS)

  # Snapshot libzmq's sanitizer options
  snapshot_cache_var(ENABLE_ASAN)
  snapshot_cache_var(ENABLE_TSAN)
  snapshot_cache_var(ENABLE_UBSAN)
  snapshot_cache_var(ENABLE_CURVE)
  snapshot_cache_var(WITH_LIBSODIUM)
  snapshot_cache_var(WITH_LIBSODIUM_STATIC)



  # --- Translate top-level sanitizer choice to libzmq's specific options ---
  set(ENABLE_ASAN OFF CACHE BOOL "Wrapper: controlled by PYLABHUB_USE_SANITIZER" FORCE)
  set(ENABLE_TSAN OFF CACHE BOOL "Wrapper: controlled by PYLABHUB_USE_SANITIZER" FORCE)
  set(ENABLE_UBSAN OFF CACHE BOOL "Wrapper: controlled by PYLABHUB_USE_SANITIZER" FORCE)

  # Convert to lowercase for case-insensitive comparison, consistent with FindSanitizerRuntime.cmake
  string(TOLOWER "${PYLABHUB_USE_SANITIZER}" _pylabhub_sanitizer_lower)

  if(_pylabhub_sanitizer_lower STREQUAL "address")
    set(ENABLE_ASAN ON CACHE BOOL "Wrapper: controlled by PYLABHUB_USE_SANITIZER" FORCE)
    message(STATUS "[pylabhub-third-party] Enabling libzmq ENABLE_ASAN.")
  elseif(_pylabhub_sanitizer_lower STREQUAL "thread")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set(ENABLE_TSAN ON CACHE BOOL "Wrapper: controlled by PYLABHUB_USE_SANITIZER" FORCE)
      message(STATUS "[pylabhub-third-party] Enabling libzmq ENABLE_TSAN for Clang compiler.")
    else()
      # Libzmq's ENABLE_TSAN options include Clang-specific flags (-mllvm).
      # If using GCC, enabling libzmq's internal TSAN will cause build errors.
      message(WARNING "[pylabhub-third-party] PYLABHUB_USE_SANITIZER is 'Thread' but libzmq's internal ENABLE_TSAN options contain Clang-specific flags ('-mllvm'). "
                      "Not enabling libzmq's internal ThreadSanitizer for current compiler (${CMAKE_CXX_COMPILER_ID}). "
                      "Libzmq will be built without its internal TSAN. This may still work if your project applies TSAN flags globally for its own targets.")
    endif()
  elseif(_pylabhub_sanitizer_lower STREQUAL "undefinedbehavior" OR _pylabhub_sanitizer_lower STREQUAL "undefined")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set(ENABLE_UBSAN ON CACHE BOOL "Wrapper: controlled by PYLABHUB_USE_SANITIZER" FORCE)
      message(STATUS "[pylabhub-third-party] Enabling libzmq ENABLE_UBSAN for Clang compiler.")
    else()
      # Libzmq's ENABLE_UBSAN options include Clang-specific or newer GCC version flags.
      # If using GCC, enabling libzmq's internal UBSAN might cause build errors.
      message(WARNING "[pylabhub-third-party] PYLABHUB_USE_SANITIZER is 'UndefinedBehavior' but libzmq's internal ENABLE_UBSAN options contain potentially Clang-specific or newer GCC version flags ('-fsanitize=implicit-conversion', etc.). "
                      "Not enabling libzmq's internal UndefinedBehaviorSanitizer for current compiler (${CMAKE_CXX_COMPILER_ID}). "
                      "Libzmq will be built without its internal UBSAN. This may still work if your project applies UBSAN flags globally for its own targets.")
    endif()
  else()
    message(STATUS "[pylabhub-third-party] No sanitizer explicitly enabled for libzmq.")
  endif()



  # ---------------------------
  # Variant selection: static/shared
  # ---------------------------
  if(NOT DEFINED THIRD_PARTY_ZMQ_FORCE_VARIANT)
    set(THIRD_PARTY_ZMQ_FORCE_VARIANT "none")
  endif()

  if(THIRD_PARTY_ZMQ_FORCE_VARIANT STREQUAL "static")
    set(BUILD_SHARED OFF CACHE BOOL "Wrapper: prefer static libs for libzmq" FORCE)
    set(BUILD_STATIC ON CACHE BOOL "Wrapper: prefer static libs for libzmq" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Wrapper: prefer static libs (generic hint)" FORCE)
    set(ZMQ_BUILD_SHARED OFF CACHE BOOL "Wrapper: prefer static libs for libzmq (alt)" FORCE)
    set(ZMQ_STATIC ON CACHE BOOL "Wrapper: prefer static libs for libzmq (alt)" FORCE)
    message(STATUS "[pylabhub-third-party] Forcing libzmq variant = static")
  elseif(THIRD_PARTY_ZMQ_FORCE_VARIANT STREQUAL "shared")
    set(BUILD_SHARED ON CACHE BOOL "Wrapper: prefer shared libs for libzmq" FORCE)
    set(BUILD_STATIC OFF CACHE BOOL "Wrapper: prefer shared libs for libzmq" FORCE)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "Wrapper: prefer shared libs (generic hint)" FORCE)
    set(ZMQ_BUILD_SHARED ON CACHE BOOL "Wrapper: prefer shared libs for libzmq (alt)" FORCE)
    set(ZMQ_STATIC OFF CACHE BOOL "Wrapper: prefer shared libs for libzmq (alt)" FORCE)
    message(STATUS "[pylabhub-third-party] Forcing libzmq variant = shared")
  else()
    message(STATUS "third_party wrapper: not forcing libzmq variant (none)")
  endif()

  # --- Platform-specific POLLER configuration ---
  # Explicitly set the POLLER type based on the detected platform for optimal performance.
  if(NOT DEFINED POLLER OR POLLER STREQUAL "")
    if(PLATFORM_LINUX)
      set(POLLER "epoll" CACHE STRING "Optimal POLLER for Linux" FORCE)
      message(STATUS "[pylabhub-third-party] Setting POLLER to 'epoll' for Linux.")
    elseif(PLATFORM_APPLE)
      set(POLLER "kqueue" CACHE STRING "Optimal POLLER for macOS" FORCE)
      message(STATUS "[pylabhub-third-party] Setting POLLER to 'kqueue' for macOS.")
    endif()
    # For Windows, libzmq's internal CMake already handles intelligent defaults (e.g., epoll on modern MSVC).
    # We will let its internal logic determine the best POLLER for Windows if not explicitly set by user.
  endif()

  # ---------------------------
  # Disable upstream tests if requested by wrapper policy (do not set global BUILD_TESTS)
  # ---------------------------
  if(THIRD_PARTY_DISABLE_TESTS)
    set(ZMQ_BUILD_TESTS OFF CACHE BOOL "Wrapper: disable libzmq tests only" FORCE)
    set(ZMQ_TESTS OFF CACHE BOOL "Wrapper: disable libzmq tests (alt)" FORCE)
    message(STATUS "[pylabhub-third-party] Disabling libzmq tests (ZMQ_BUILD_TESTS=OFF)")
  endif()

  # ---------------------------
  # Precompiled header policy (scoped)
  # ---------------------------
  if(NOT DEFINED THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(THIRD_PARTY_ALLOW_UPSTREAM_PCH OFF)
  endif()

  if(NOT THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(ENABLE_PRECOMPILED OFF CACHE BOOL "Wrapper: disable libzmq precompiled headers" FORCE)
    set(ZMQ_USE_PRECOMPILED_HEADER OFF CACHE BOOL "Wrapper: disable libzmq precompiled headers (alt)" FORCE)
    set(ZMQ_USE_PCH OFF CACHE BOOL "Wrapper: disable libzmq precompiled headers (alt2)" FORCE)
    set(ENABLE_PRECOMPILED_HEADER OFF CACHE BOOL "Wrapper: disable libzmq precompiled headers (alt3)" FORCE)
    message(STATUS "[pylabhub-third-party] Disabling libzmq PCH")
  else()
    message(STATUS "[pylabhub-third-party] Allowing libzmq to use PCH if its build requests it")
  endif()

  # Extra note for MSVC + Ninja where PCH can cause duplicate-rule errors
  if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    if(NOT THIRD_PARTY_ALLOW_UPSTREAM_PCH AND NOT THIRD_PARTY_FORCE_ALLOW_PCH)
      message(STATUS "[pylabhub-third-party] MSVC+Ninja detected; keeping upstream PCH disabled by default to avoid duplicate-rule errors.")
    elseif(THIRD_PARTY_FORCE_ALLOW_PCH)
      message(WARNING "[pylabhub-third-party] MSVC+Ninja detected but you forced allowing upstream PCH (may see ninja: 'multiple rules generate precompiled.hpp').")
    endif()
  endif()

  # ---------------------------
  # Minimize upstream libzmq build (turn off docs, examples, optional transports, etc.)
  # ---------------------------
  set(WITH_PERF_TOOL OFF CACHE BOOL "Wrapper: disable libzmq perf tools" FORCE)
  set(WITH_DOCS OFF CACHE BOOL "Wrapper: disable libzmq docs" FORCE)
  set(ZMQ_BUILD_FRAMEWORK OFF CACHE BOOL "Wrapper: disable libzmq framework packaging" FORCE)

  # --- Translate top-level tunables to libzmq-specific options ---
  # Use the PYLABHUB_ZMQ_* variables to control the build. The `FORCE` ensures
  # our policy is respected within the isolated build scope.
  set(WITH_OPENPGM ${PYLABHUB_ZMQ_WITH_OPENPGM} CACHE BOOL "Wrapper: controlled by PYLABHUB_ZMQ_WITH_OPENPGM" FORCE)
  set(WITH_NORM ${PYLABHUB_ZMQ_WITH_NORM} CACHE BOOL "Wrapper: controlled by PYLABHUB_ZMQ_WITH_NORM" FORCE)
  set(WITH_VMCI ${PYLABHUB_ZMQ_WITH_VMCI} CACHE BOOL "Wrapper: controlled by PYLABHUB_ZMQ_WITH_VMCI" FORCE)

  # --- Force enable CurveZMQ security ---
  # This is a core requirement for the pylabhub broker design.
  message(STATUS "[pylabhub-third-party] Forcing ENABLE_CURVE=ON and WITH_LIBSODIUM=ON for ZeroMQ security.")
  set(ENABLE_CURVE ON CACHE BOOL "pylabhub: required for secure broker" FORCE)
  set(WITH_LIBSODIUM ON CACHE BOOL "pylabhub: required for CurveZMQ" FORCE)
  set(WITH_LIBSODIUM_STATIC ON CACHE BOOL "pylabhub: linking libsodium statically" FORCE)

  set(ZMQ_BUILD_EXAMPLES OFF CACHE BOOL "Wrapper: disable libzmq example binaries" FORCE)
  set(ZMQ_BUILD_TOOLS OFF CACHE BOOL "Wrapper: disable libzmq helper tools" FORCE)

  # --- Print summary of effective settings for visibility ---
  message("")
  message(STATUS "=========================================================")
  message(STATUS "[pylabhub-third-party] Applying the following options for libzmq build:")
  message(STATUS "  Build variant:")
  message(STATUS "    - BUILD_SHARED_LIBS: ${BUILD_SHARED_LIBS}")
  message(STATUS "    - BUILD_STATIC:      ${BUILD_STATIC}")
  message(STATUS "    - BUILD_SHARED:      ${BUILD_SHARED}")
  message(STATUS "  Features:")
  message(STATUS "    - ZMQ_BUILD_TESTS:   ${ZMQ_BUILD_TESTS}")
  message(STATUS "    - WITH_DOCS:         ${WITH_DOCS}")
  message(STATUS "    - WITH_DOC:          ${WITH_DOC}")
  message(STATUS "    - ZMQ_BUILD_EXAMPLES:${ZMQ_BUILD_EXAMPLES}")
  message(STATUS "  Optional Transports:")
  message(STATUS "    - WITH_OPENPGM:      ${WITH_OPENPGM}")
  message(STATUS "    - WITH_NORM:         ${WITH_NORM}")
  message(STATUS "    - WITH_VMCI:         ${WITH_VMCI}")
  message(STATUS "  Security:")
  message(STATUS "    - ENABLE_CURVE:      ${ENABLE_CURVE} (Forced ON)")
  message(STATUS "    - WITH_LIBSODIUM:    ${WITH_LIBSODIUM} (Forced ON)")
  message(STATUS "    - WITH_LIBSODIUM_STATIC: ${WITH_LIBSODIUM_STATIC} (Forced ON)")
  message(STATUS "    - SODIUM_INCLUDE_DIRS:   ${SODIUM_INCLUDE_DIRS}")
  message(STATUS "    - SODIUM_LIBRARIES:      ${SODIUM_LIBRARIES}")
  message(STATUS "  Precompiled Headers:")
  message(STATUS "    - ENABLE_PRECOMPILED:${ENABLE_PRECOMPILED}")
  message(STATUS "  Sanitizers (from top-level PYLABHUB_USE_SANITIZER):")
  message(STATUS "    - ENABLE_ASAN:       ${ENABLE_ASAN}")
  message(STATUS "    - ENABLE_TSAN:       ${ENABLE_TSAN}")
  message(STATUS "    - ENABLE_UBSAN:      ${ENABLE_UBSAN}")
  message(STATUS "  POLLER:              ${POLLER}")
  message(STATUS "=========================================================")
  message("")

  # ---------------------------
  # Add subproject (scoped)
  # ---------------------------
  add_subdirectory(libzmq EXCLUDE_FROM_ALL)

  # --- 4. Discover the canonical concrete target ---
  # Find the real, installable target for the libzmq library, ignoring aliases.
  # ---------------------------
  set(_zmq_canonical_target "")
  set(_zmq_candidates "libzmq-static;libzmq-shared;libzmq;zmq;zmq::zmq")

  foreach(_cand IN LISTS _zmq_candidates)
    if(TARGET "${_cand}" AND NOT TARGET "${_cand}" STREQUAL "INTERFACE")
      _resolve_alias_to_concrete("${_cand}" _zmq_canonical_target)
      message(STATUS "[pylabhub-third-party] Found canonical libzmq target: ${_zmq_canonical_target} (from candidate '${_cand}')")
      break()
    endif()
  endforeach()

  if(_zmq_canonical_target)
    target_link_libraries(${_zmq_canonical_target} PUBLIC pylabhub::third_party::sodium)
  endif()

  # --- 5. Create wrapper target and define usage requirements ---
  # This provides a stable `pylabhub::third_party::zmq` for consumers.
  # ---------------------------
  _expose_wrapper(pylabhub_libzmq pylabhub::third_party::libzmq)

  # --- 5b. Install the wrapper target for export ---
  # This ensures that pylabhub_libzmq is included in the pylabhubTargets export set,
  # allowing other projects (or other targets within this project) to find its
  # interface properties (like include directories and link libraries).
  install(TARGETS pylabhub_libzmq EXPORT pylabhubTargets)

  if(_zmq_canonical_target)
    # Add a dependency to ensure libsodium is built before libzmq.
    add_dependencies(${_zmq_canonical_target} build_prerequisites)

    # Binary library case: Link the wrapper to the concrete target.
    target_link_libraries(pylabhub_libzmq INTERFACE ${_zmq_canonical_target})
    message(STATUS "[pylabhub-third-party] Linking pylabhub_libzmq -> ${_zmq_canonical_target}")
  else()
    # This should not happen for libzmq unless the build is misconfigured.
    message(WARNING "[pylabhub-third-party] No canonical binary target found for libzmq. Consumers may fail to link.")
    # Manually add include directory as a fallback.
    target_include_directories(pylabhub_libzmq INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libzmq/include>
      $<INSTALL_INTERFACE:include>
    )
  endif()

  # --- 6. Stage artifacts for installation ---
  # Add custom commands to copy headers and libs to the staging directory.
  # ---------------------------
  if(THIRD_PARTY_INSTALL)
    message(STATUS "[pylabhub-third-party] Scheduling libzmq artifacts for staging...")

    # Stage the header directory
    pylabhub_stage_headers(
      DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/include"
      SUBDIR "zmq"
    )

    # Stage the library file, if a concrete target was found
    if(_zmq_canonical_target)
      pylabhub_stage_libraries(TARGETS ${_zmq_canonical_target})
    endif()
  else()
    message(STATUS "[pylabhub-third-party] THIRD_PARTY_INSTALL is OFF; skipping staging for libzmq.")
  endif()

  # --- 7. Restore cache variables ---
  # Prevent settings from leaking to other sub-projects.
  # ---------------------------
  restore_cache_var(BUILD_SHARED BOOL)
  restore_cache_var(BUILD_STATIC BOOL)
  restore_cache_var(BUILD_SHARED_LIBS BOOL)
  restore_cache_var(ZMQ_BUILD_TESTS BOOL)
  restore_cache_var(BUILD_TESTS BOOL)
  restore_cache_var(ENABLE_PRECOMPILED BOOL)

  restore_cache_var(ZMQ_USE_PRECOMPILED_HEADER BOOL)
  restore_cache_var(ZMQ_USE_PCH BOOL)
  restore_cache_var(ENABLE_PRECOMPILED_HEADER BOOL)

  restore_cache_var(WITH_PERF_TOOL BOOL)
  restore_cache_var(WITH_DOCS BOOL)
  restore_cache_var(WITH_DOC BOOL)
  restore_cache_var(ZMQ_BUILD_FRAMEWORK BOOL)
  restore_cache_var(POLLER STRING)
  restore_cache_var(WITH_OPENPGM BOOL)
  restore_cache_var(WITH_NORM BOOL)
  restore_cache_var(WITH_VMCI BOOL)
  restore_cache_var(ZMQ_BUILD_EXAMPLES BOOL)
  restore_cache_var(ZMQ_BUILD_TOOLS BOOL)

  restore_cache_var(ENABLE_ASAN BOOL)
  restore_cache_var(ENABLE_TSAN BOOL)
  restore_cache_var(ENABLE_UBSAN BOOL)
  restore_cache_var(ENABLE_CURVE BOOL)
  restore_cache_var(WITH_LIBSODIUM BOOL)
  restore_cache_var(WITH_LIBSODIUM_STATIC BOOL)

  message(STATUS "[pylabhub-third-party] libzmq configuration complete.")
else()
  message(WARNING "[pylabhub-third-party] libzmq submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/libzmq. Skipping configuration.")
endif()
