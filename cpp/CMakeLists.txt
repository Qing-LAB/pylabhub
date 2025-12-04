# ===========================================================================
# Top-Level CMakeLists.txt for the pyLabHub C++ Project
#
# This file is the main entry point for the C++ build system. It orchestrates the
# entire process, from dependency management to final installation, based on a
# unified staging architecture.
#
# ---------------------------------------------------------------------------
# HOW TO USE THIS BUILD SYSTEM
# ---------------------------------------------------------------------------
#
# This system is designed around a "staging" directory (`build/stage/`). All
# artifacts are first copied here, creating a complete, runnable package. The
# final `install` step is just a simple copy of this directory.
#
# --- Exposed User Options ---
#   -DBUILD_TESTS=ON/OFF              (Default: ON)   Build the test suite.
#   -DBUILD_XOP=ON/OFF                (Default: ON)   Build the IgorXOP module.
#   -DPYLABHUB_BUILD_SHARED=ON/OFF    (Default: OFF)  Build corelib as shared/static.
#   -DTHIRD_PARTY_INSTALL=ON/OFF      (Default: ON)   Stage third-party libraries.
#   -DPYLABHUB_CREATE_INSTALL_TARGET=ON/OFF (Default: ON)   Enable the `install` target.
#   -DCMAKE_INSTALL_PREFIX=<path>     (Default: /usr/local or C:/Program Files)
#
# --- Common Scenarios (from the `cpp` directory) ---
#
# 1.  Standard Development Build (for local testing):
#     Builds everything, stages all artifacts to `build/stage/`.
#
#     $ cmake -S . -B build
#     $ cmake --build build
#
#     After this, you can run executables directly from `build/stage/bin/`.
#
# 2.  Build and Run Tests:
#     Ensures tests are built and runnable via CTest.
#
#     $ cmake -S . -B build -DBUILD_TESTS=ON
#     $ cmake --build build
#     $ cd build
#     $ ctest
#
# 3.  Create a Full Distributable Package:
#     Stages all components (including third-party libs) and installs them
#     to the specified prefix.
#
#     $ cmake -S . -B build -DTHIRD_PARTY_INSTALL=ON \
#           -DPYLABHUB_CREATE_INSTALL_TARGET=ON \
#           -DCMAKE_INSTALL_PREFIX=../install
#     $ cmake --build build
#     $ cmake --install build
#
#     The `../install` directory will now contain the complete package.
#
# 4.  Minimal Build (Core project only, no tests, no install):
#     Useful for quick compilation checks or CI.
#
#     $ cmake -S . -B build -DBUILD_TESTS=OFF -DTHIRD_PARTY_INSTALL=OFF \
#           -DPYLABHUB_CREATE_INSTALL_TARGET=OFF
#     $ cmake --build build
#
# ---------------------------------------------------------------------------
# Core Design Principles:
#
# 1.  **Unified Staging**: All artifacts are copied to `${PYLABHUB_STAGING_DIR}`.
# 2.  **Modularity**: The build is broken into logical components (`cmake/`,
#     `third_party/`, `src/`, `tests/`).
# 3.  **Clear Separation**: Core components are always staged. Staging of
#     third-party libs, tests, and the final `install` target are optional.
# ===========================================================================
cmake_minimum_required(VERSION 3.18)

project(pylabhub-cpp VERSION 0.1 LANGUAGES C CXX)

# --- 1. Global Setup and Helper Modules ---
# This section sets up the fundamental build environment.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(PlatformAndCompiler) # Handles platform detection and compiler flags.
include(ToplevelOptions)     # Defines top-level options like BUILD_TESTS.
include(StageHelpers)        # Provides `pylabhub_stage_*` functions.
find_package(Threads REQUIRED)

message(STATUS "")
message(STATUS "===============================================================")
message(STATUS "Top-Level Project: ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "Build Type:          ${CMAKE_BUILD_TYPE}")
message(STATUS "Generator:           ${CMAKE_GENERATOR}")
message(STATUS "Compiler:            ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "===============================================================")
message(STATUS "")

# --- 2. Staging Infrastructure ---
# This section defines the unified staging directory and the master target
# that drives the entire artifact collection process.

# `PYLABHUB_STAGING_DIR`: The single location within the build directory
# (e.g., `build/stage`) where all artifacts will be copied. This directory
# will mirror the final installation layout.
set(PYLABHUB_STAGING_DIR "${CMAKE_BINARY_DIR}/stage" CACHE PATH "Root directory for staging all project artifacts.")

# `stage_all`: A global custom target that serves as a dependency hook. Other
# sub-projects (like `third_party` and `IgorXOP`) add their staging commands
# as dependencies to this target. Building `stage_all` ensures that all
# necessary artifacts are collected.
add_custom_target(stage_all COMMENT "Staging all project dependencies and artifacts to ${PYLABHUB_STAGING_DIR}")
message(STATUS "Unified staging directory set to: ${PYLABHUB_STAGING_DIR}")

# --- 3. Configure Third-Party Dependencies ---
# This step configures all external libraries. The `third_party/CMakeLists.txt`
# script orchestrates the individual wrapper scripts (`fmt.cmake`, etc.),
# which create the clean `pylabhub::third_party::*` targets we use later.
message(STATUS "--- Configuring Third-Party Dependencies ---")
add_subdirectory(third_party)
message(STATUS "--- Third-Party Configuration Complete ---")
message(STATUS "")

# --- 4. Build Main Project Libraries and Executables ---
message(STATUS "--- Configuring Main Project Targets ---")

# --- 3.1. pylabhub-corelib ---
# This is the core library containing common utilities.
file(GLOB_RECURSE CORELIB_SOURCES CONFIGURE_DEPENDS "src/util/*.cpp" "src/fileutil/*.cpp")

if(PYLABHUB_BUILD_SHARED)
  add_library(pylabhub-corelib SHARED ${CORELIB_SOURCES})
else()
  add_library(pylabhub-corelib STATIC ${CORELIB_SOURCES})
endif()

# Expose a stable alias for other project components to use.
add_library(pylabhub::corelib ALIAS pylabhub-corelib)
message(STATUS "Configured target: pylabhub-corelib (alias: pylabhub::corelib)")

# Define public include directories for the core library.
target_include_directories(pylabhub-corelib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Link corelib against the third-party targets it needs.
# The third-party framework ensures these targets are always available.
target_link_libraries(pylabhub-corelib
  PUBLIC
    pylabhub::third_party::nlohmann_json # Public because corelib headers may include it.
    pylabhub::third_party::fmt           # Public because static consumers need to link to it.
  PRIVATE
    pylabhub::third_party::zmq
    Threads::Threads
)

# Set standard properties for the core library.
target_compile_features(pylabhub-corelib PUBLIC cxx_std_17)
set_target_properties(pylabhub-corelib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(PYLABHUB_BUILD_SHARED AND MSVC)
  set_target_properties(pylabhub-corelib PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# --- 3.2. pylabhub-shell (Main Executable) ---
# The logical target name is `pylabhub-shell`.
add_executable(pylabhub-shell src/coreshell.cpp)
add_executable(pylabhub::shell ALIAS pylabhub-shell)
target_link_libraries(pylabhub-shell PRIVATE pylabhub::corelib)

# Set the output file name to `hubshell` (.exe on Windows).
set_target_properties(pylabhub-shell PROPERTIES OUTPUT_NAME "hubshell")
message(STATUS "Configured target: pylabhub-shell (alias: pylabhub::shell)")

message(STATUS "--- Main Project Configuration Complete ---")
message(STATUS "")

# --- 5. Optional Sub-Projects ---
message(STATUS "--- Configuring Optional Sub-Projects ---")

# --- 4.1. IgorXOP ---
if(BUILD_XOP)
  add_subdirectory(src/IgorXOP)
else()
  message(STATUS "Skipping IgorXOP sub-project (BUILD_XOP is OFF).")
endif()

# --- 4.2. Tests ---
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
else()
  message(STATUS "Skipping tests sub-project (BUILD_TESTS is OFF).")
endif()

message(STATUS "--- Optional Sub-Projects Configuration Complete ---")
message(STATUS "")

# --- 6. Staging for Main Project Targets ---
# This section defines the post-build commands that copy the core project
# artifacts into the unified staging directory. This staging is UNCONDITIONAL
# because `pylabhub-corelib` and `pylabhub-shell` are essential components
# of the project.

# The `add_custom_command(TARGET ... POST_BUILD ...)` pattern ensures that
# artifacts are copied immediately after they are successfully built.

# Stage the main executable to `stage/bin/`.
add_custom_command(TARGET pylabhub-shell POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/bin"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:pylabhub-shell>" "${PYLABHUB_STAGING_DIR}/bin/"
  # Copy the runtime dependencies (DLLs) of the executable to the bin directory.
  # This is the correct place to use TARGET_RUNTIME_DLLS.
  COMMAND ${CMAKE_COMMAND}
    -D TARGET_NAME=pylabhub-shell
    -D DEST_DIR=${PYLABHUB_STAGING_DIR}/bin
    -D "RUNTIME_DEPS=$<TARGET_RUNTIME_DLLS:pylabhub-shell>"
    -P "${CMAKE_SOURCE_DIR}/cmake/StageRuntimeDeps.cmake"
  COMMENT "Staging hubshell and its runtime dependencies to ${PYLABHUB_STAGING_DIR}/bin"
  VERBATIM
)

# Stage the core library (and its runtime dependencies on Windows)
add_custom_command(TARGET pylabhub-corelib POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/lib"
  # Only copy the library file itself. The runtime dependencies are handled by the executable target.
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:pylabhub-corelib>" "${PYLABHUB_STAGING_DIR}/lib/"
  COMMENT "Staging pylabhub-corelib to ${PYLABHUB_STAGING_DIR}/lib"
)

# Stage the public headers
add_custom_command(TARGET pylabhub-corelib POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/include"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/include/" "${PYLABHUB_STAGING_DIR}/include/"
  COMMENT "Staging public headers to ${PYLABHUB_STAGING_DIR}/include"
)

# Make the global `stage_all` target depend on the core targets. This ensures
# that building `stage_all` will first build these components before any
# dependent staging commands are run.
add_dependencies(stage_all pylabhub-shell pylabhub-corelib)

# --- 7. Final Installation Step ---
# This section defines the `install` target. Its creation is controlled by the
# `PYLABHUB_CREATE_INSTALL_TARGET` option, allowing developers to disable it
# for build-only scenarios (e.g., in CI).
if(PYLABHUB_CREATE_INSTALL_TARGET)
  # The `install` command is now a simple, atomic operation that copies the
  # entire, fully-populated staging directory to the installation prefix
  # (`CMAKE_INSTALL_PREFIX`).
  install(DIRECTORY "${PYLABHUB_STAGING_DIR}/"
    DESTINATION "."
    COMPONENT "pylabhub_runtime"
  )
  message(STATUS "Installation target enabled. Run 'cmake --install .' to copy staged artifacts.")
endif()

# --- 8. Final Configuration Summary ---
# This block prints a clear summary of the selected build options, providing
# immediate feedback to the developer on how the project is configured.
message(STATUS "==============================================")
message(STATUS "pyLabHub Configuration Summary")
message(STATUS "----------------------------------------------")

if(PYLABHUB_BUILD_SHARED)
  set(_corelib_type "SHARED")
else()
  set(_corelib_type "STATIC")
endif()
if(BUILD_TESTS)
  set(_build_tests "ON")
else()
  set(_build_tests "OFF")
endif()
if(BUILD_XOP)
  set(_build_xop "ON")
else()
  set(_build_xop "OFF")
endif()
if(PYLABHUB_CREATE_INSTALL_TARGET)
  set(_install_enabled "ON")
else()
  set(_install_enabled "OFF")
endif()

message(STATUS "  Install Prefix:      ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Build Type:          ${CMAKE_BUILD_TYPE}")
message(STATUS "  Core Library Type:   ${_corelib_type}")
message(STATUS "  Build Tests:         ${_build_tests}")
message(STATUS "  Build IgorXOP:       ${_build_xop}")
if(THIRD_PARTY_INSTALL)
message(STATUS "  3rd-Party Staging:   ON")
else()
message(STATUS "  3rd-Party Staging:   OFF")
endif()
message(STATUS "  Create Install Target: ${PYLABHUB_CREATE_INSTALL_TARGET} (from ${PYLABHUB_STAGING_DIR})")
message(STATUS "==============================================")
