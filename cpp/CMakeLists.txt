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
#   -DFORCE_USE_CLANG_ON_APPLE=ON/OFF (Default: ON)   Ensure Apple toolchain is used on macOS.
#   -DCMAKE_INSTALL_PREFIX=<path>     (Default: /usr/local or C:/Program Files)
#
# --- Common Scenarios (from the `cpp` directory) ---
#
# 1.  Standard Development Build (Makefile or Ninja):
#     Generates build files for a command-line build system like Make or Ninja.
#     This is the most common workflow for day-to-day development.
#
#     $ cmake -S . -B build
#     $ cmake --build build
#
#     After this, you can run executables directly from `build/stage/bin/`.
#
# 2.  Xcode Project Generation and Build:
#     Generates an `.xcodeproj` file for use with Xcode or `xcodebuild`.
#     NOTE: This requires a clean environment to ensure the correct Apple toolchain
#     is used, especially if you have other compilers (like from Homebrew) installed.
#
#     a. Generate the Xcode project:
#        $ cmake -S . -B build_xcode -G Xcode
#
#     b. Build from the command line (recommended):
#        The `CC=` and `CXX=` prefixes ensure any conflicting environment variables are ignored.
#        $ CC= CXX= xcodebuild -project build_xcode/pylabhub-cpp.xcodeproj
#
#     c. Alternatively, open `build_xcode/pylabhub-cpp.xcodeproj` in the Xcode IDE and build from there.
#
# 3.  Create a Full Distributable Package:
#     This performs a full staging and then copies the result to the final installation directory.
#
#     $ cmake -S . -B build -DTHIRD_PARTY_INSTALL=ON \
#           -DPYLABHUB_CREATE_INSTALL_TARGET=ON \
#           -DCMAKE_INSTALL_PREFIX=../install
#     $ cmake --build build --target stage_all
#     $ cmake --install build
#
# ---------------------------------------------------------------------------
# Core Design Principles:
#
# Our architecture is built on a foundation of modern CMake practices,
# emphasizing clarity, robustness, and maintainability.
#
# 1.  **Unified Staging**: The cornerstone of the design is the unified
#     staging directory (`${PYLABHUB_STAGING_DIR}`). All build
#     artifacts—executables, libraries, headers, and bundles—are copied into
#     this single location within the build tree. This creates a self-contained,
#     runnable version of the project that mirrors the final installation
#     layout, making local development and testing simple and reliable.
# 2.  **Isolation & Abstraction**: Third-party dependencies are configured
#     in isolated "sandboxes" to prevent build settings from "leaking."
#     Consumers link against stable, namespaced `ALIAS` targets, abstracting
#     away underlying build details.
# 3.  **Top-Down Control & Modularity**: The build system is broken into
#     logical, single-responsibility modules. High-level build policies are
#     defined as user-facing `CACHE` options.
# 4.  **Clear Separation of Concerns**: Core components are always staged.
#     Third-party libraries and optional components (`tests`) have their staging
#     and building controlled by dedicated options.
# 5.  **Installation**: The final `install` target performs a direct copy
#     of the fully-populated staged directory, providing a clean separation
#     between development builds and distributable packages.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)

# ===========================================================================
# PHASE 1: Pre-Project Toolchain Configuration
#
# This phase runs BEFORE the `project()` command. Its sole purpose is to
# influence which compiler and toolchain CMake will select. Logic here cannot
# depend on compiler-specific information (like CMAKE_CXX_COMPILER_ID) because
# the compiler has not been tested yet. We must use general system variables
# like CMAKE_HOST_SYSTEM_NAME.
# ===========================================================================

# Include all user-facing options first, so they are available for the logic below.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ToplevelOptions)

# On macOS, ensure we use the Apple-provided toolchain.
if(FORCE_USE_CLANG_ON_APPLE AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  # For Makefile/Ninja generators, explicitly setting the compiler path
  # prevents CMake from finding conflicting versions (e.g., from Homebrew).
  if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    find_program(_clang_path clang HINTS /usr/bin)
    find_program(_clangpp_path clang++ HINTS /usr/bin)
    if(_clang_path AND _clangpp_path)
      set(CMAKE_C_COMPILER "${_clang_path}" CACHE FILEPATH "C compiler" FORCE)
      set(CMAKE_CXX_COMPILER "${_clangpp_path}" CACHE FILEPATH "C++ compiler" FORCE)
      message(STATUS "Top-level: forcing clang/clang++ on macOS: ${_clang_path}, ${_clangpp_path}")
    else()
      message(WARNING "Top-level: clang not found on PATH. Consider installing Xcode or CommandLineTools.")
    endif()
  # For the Xcode generator, we must NOT set CMAKE_C_COMPILER directly. Instead, we
  # unset environment variables that might confuse `xcodebuild` at build time.
  else()
    unset(ENV{CC})
    unset(ENV{CXX})
    message(STATUS "Xcode generator: Unsetting CC and CXX environment variables to allow default toolchain selection.")
  endif()
endif()

# ===========================================================================
# PHASE 2: Project Definition and Toolchain Detection
#
# The `project()` command is the most critical part of the configuration.
# It triggers CMake to find and test the compilers for the specified languages.
# After this command, variables like CMAKE_CXX_COMPILER_ID are available.
# ===========================================================================
project(pylabhub-cpp VERSION 0.1 LANGUAGES C CXX)

# ===========================================================================
# PHASE 3: Post-Project Configuration
#
# All logic from this point forward can react to the chosen compiler and
# platform. This is the correct place to set compiler-specific flags,
# include helper modules that need the compiler ID, and define targets.
# ===========================================================================

# --- 3.1. Global Setup and Helper Modules ---
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(PlatformAndCompiler) # Handles platform detection and compiler flags.
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

# --- 3.2. Staging Infrastructure ---
# This section defines the unified staging directory and the master target
# that drives the entire artifact collection process.
set(PYLABHUB_STAGING_DIR "${CMAKE_BINARY_DIR}/stage" CACHE PATH "Root directory for staging all project artifacts.")
add_custom_target(stage_all
    COMMAND ${CMAKE_COMMAND} -E echo " "
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------------------------------"
    COMMAND ${CMAKE_COMMAND} -E echo "Staging complete. The project is now ready for installation."
    COMMAND ${CMAKE_COMMAND} -E echo "To install to ${CMAKE_INSTALL_PREFIX}, run: cmake --install ."
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------------------------------"
    COMMENT "Staging all project dependencies and artifacts to ${PYLABHUB_STAGING_DIR}"
)
message(STATUS "Unified staging directory set to: ${PYLABHUB_STAGING_DIR}")

# --- 3.3. Configure All Sub-Projects ---
message(STATUS "--- Configuring Third-Party Dependencies ---")
add_subdirectory(third_party)
message(STATUS "--- Third-Party Configuration Complete ---")
message(STATUS "")

message(STATUS "--- Configuring Main Project Targets ---")
add_subdirectory(src)
message(STATUS "--- Main Project Configuration Complete ---")
message(STATUS "")

message(STATUS "--- Configuring Optional Sub-Projects ---")
if(BUILD_XOP)
  add_subdirectory(src/IgorXOP)
else()
  message(STATUS "Skipping IgorXOP sub-project (BUILD_XOP is OFF).")
endif()
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
else()
  message(STATUS "Skipping tests sub-project (BUILD_TESTS is OFF).")
endif()
message(STATUS "--- Optional Sub-Projects Configuration Complete ---")
message(STATUS "")

# --- 3.4. Staging Dependencies ---
# The global `stage_all` target depends on the individual staging targets
# from each sub-project. This ensures all artifacts are staged correctly
# before the final message is printed.
add_dependencies(stage_all stage_core_artifacts)

# If the XOP is being built, make sure its staging target is part of the global `stage_all`.
if(BUILD_XOP)
  # This target is defined in src/IgorXOP/CMakeLists.txt
  add_dependencies(stage_all stage_pylabhubxop)
endif()

# --- 3.5. Final Installation Step ---
# Defines the `install` target, which simply copies the fully populated
# staging directory to the final install prefix.
if(PYLABHUB_CREATE_INSTALL_TARGET)
  install(DIRECTORY "${PYLABHUB_STAGING_DIR}/"
    DESTINATION "."
    COMPONENT "pylabhub_runtime"
  )
  message(STATUS "Installation target enabled. Run 'cmake --install' to copy staged artifacts.")
endif()

# --- 3.6. Final Configuration Summary ---
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
message(STATUS "")
message(STATUS "Build configuration is complete.")
message(STATUS "1. To build all targets, run: cmake --build <your_build_directory>")
message(STATUS "2. To stage all artifacts to '${PYLABHUB_STAGING_DIR}', run: cmake --build <your_build_directory> --target stage_all")
message(STATUS "")