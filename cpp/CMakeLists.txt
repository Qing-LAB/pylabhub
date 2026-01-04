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
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")


# Include all user-facing options first, so they are available for the logic below.
include(ToplevelOptions)

include(ProjectToolChainSetup)

# ===========================================================================
# PHASE 2: Project Definition and Toolchain Detection
#
# The `project()` command is the most critical part of the configuration.
# It triggers CMake to find and test the compilers for the specified languages.
# After this command, variables like CMAKE_CXX_COMPILER_ID are available.
# ===========================================================================
project(pylabhub-cpp VERSION 0.1 LANGUAGES C CXX)

# --- Set RPATH for staged/installed executables ---
# This ensures that executables can find shared libraries in a relative path.
# Binaries are in 'bin/', libraries are in 'lib/'. The RPATH should point
# from the executable to the library directory.
# For Linux/macOS, $ORIGIN is a linker directive that resolves to the executable's path.
if(UNIX)
  if(APPLE)
    # On macOS, @loader_path is similar to $ORIGIN but is correctly handled by dyld.
    # We also need to ensure that the executable is marked as allowing rpath.
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
    set(CMAKE_MACOSX_RPATH ON)
  else()
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
  endif()
endif()
# CMAKE_BUILD_WITH_INSTALL_RPATH tells CMake to use the install RPATH for
# the build tree as well. This makes the executables in `build/stage/bin`
# runnable without needing `LD_LIBRARY_PATH`.
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)

include(GNUInstallDirs)

# ===========================================================================
# PHASE 3: Post-Project Configuration
#
# All logic from this point forward can react to the chosen compiler and
# platform. This is the correct place to set compiler-specific flags,
# include helper modules that need the compiler ID, and define targets.
# ===========================================================================

# --- 3.1. Global Setup and Helper Modules ---
include(PlatformAndCompiler) # Handles platform detection and compiler flags.
include(StageHelpers)        # Provides `pylabhub_stage_*` functions.
find_package(Threads REQUIRED)

message(STATUS "")
message(STATUS "===============================================================")
message(STATUS "Top-Level Project: ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "Build Type:          ${CMAKE_BUILD_TYPE}")
message(STATUS "Generator:           ${CMAKE_GENERATOR}")
message(STATUS "Compiler:            ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# Display the platform detected by PlatformAndCompiler.cmake. This makes the
# build configuration more transparent and confirms which platform-specific
# logic will be activated.
if(PLATFORM_WIN64)
  set(_platform_string "Windows x64")
elseif(PLATFORM_APPLE)
  set(_platform_string "Apple macOS")
elseif(PLATFORM_LINUX)
  set(_platform_string "Linux")
else()
  set(_platform_string "Unknown")
endif()
message(STATUS "Platform:            ${_platform_string}")
message(STATUS "===============================================================")
message(STATUS "")

# --- 3.2. Staging Infrastructure ---
# This section defines the unified staging directory and the master target
# that drives the entire artifact collection process.
set(PYLABHUB_STAGING_DIR "${CMAKE_BINARY_DIR}/stage" CACHE PATH "Root directory for staging all project artifacts.")

# Create a dedicated target to initialize all staging directories. This
# prevents race conditions and file/directory name conflicts by ensuring
# the directory structure exists before any artifacts are copied.
add_custom_target(create_staging_dirs
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/include"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/IgorXOP"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/docs"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/tools"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/config"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/tests"
    COMMENT "Initializing staging directories"
)

# --- Stage MSVC ASan Runtime DLLs (if applicable) ---
if(MSVC AND PYLABHUB_USE_SANITIZER STREQUAL "Address")
    get_filename_component(MSVC_COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)

    set(ASAN_DLL_DEBUG "clang_rt.asan_dbg_dynamic-x86_64.dll")
    set(ASAN_DLL_RELEASE "clang_rt.asan_dynamic-x86_64.dll")

    # Verify that the ASan DLLs exist in the compiler's directory before staging.
    if(EXISTS "${MSVC_COMPILER_DIR}/${ASAN_DLL_DEBUG}" AND EXISTS "${MSVC_COMPILER_DIR}/${ASAN_DLL_RELEASE}")
        add_custom_command(TARGET create_staging_dirs POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSVC_COMPILER_DIR}/${ASAN_DLL_DEBUG}"
                "${PYLABHUB_STAGING_DIR}/bin/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSVC_COMPILER_DIR}/${ASAN_DLL_RELEASE}"
                "${PYLABHUB_STAGING_DIR}/bin/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSVC_COMPILER_DIR}/${ASAN_DLL_DEBUG}"
                "${PYLABHUB_STAGING_DIR}/tests/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSVC_COMPILER_DIR}/${ASAN_DLL_RELEASE}"
                "${PYLABHUB_STAGING_DIR}/tests/"
            COMMENT "Staging MSVC ASan runtime DLLs"
            VERBATIM
        )
        message(STATUS "Found and will stage MSVC ASan runtime DLLs from: ${MSVC_COMPILER_DIR}")
    else()
        message(WARNING "Could not find MSVC ASan runtime DLLs in the compiler directory: ${MSVC_COMPILER_DIR}. ASan-enabled tests may fail to run.")
    endif()
endif()

# --- Stage Sanitizer Runtimes (non-MSVC) ---
include(FindSanitizerRuntime)

add_custom_target(stage_all
    COMMAND ${CMAKE_COMMAND} -E echo " "
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------------------------------"
    COMMAND ${CMAKE_COMMAND} -E echo "Staging complete. The project is now ready for installation."
    COMMAND ${CMAKE_COMMAND} -E echo "To install to ${CMAKE_INSTALL_PREFIX}, run: cmake --install ."
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------------------------------"
    COMMENT "Staging all project dependencies and artifacts to ${PYLABHUB_STAGING_DIR}"
)
# This custom target serves as a hook for all core project artifacts (libraries,
# executables) to attach their staging commands. It is a dependency of `stage_all`.
add_custom_target(stage_core_artifacts)

# Ensure that the core staging process and the overall staging process
# wait for the directories to be created first.
add_dependencies(stage_core_artifacts create_staging_dirs)
add_dependencies(stage_all create_staging_dirs)

message(STATUS "Unified staging directory set to: ${PYLABHUB_STAGING_DIR}")

# --- 3.3. Configure All Sub-Projects ---
message(STATUS "--- Configuring Third-Party Dependencies ---")
add_subdirectory(third_party)
message(STATUS "--- Third-Party Configuration Complete ---")
message(STATUS "")

message(STATUS "--- Configuring Main Project Targets ---")
# The 'src' directory is the root of our source tree. Its own CMakeLists.txt
# is responsible for adding its subdirectories (like 'utils').
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

# --- 3.4. Finalize Staging Dependencies ---
# Collect all core staging targets registered by sub-projects via the global property.
get_property(core_stage_targets GLOBAL PROPERTY CORE_STAGE_TARGETS)
if(core_stage_targets)
  message(STATUS "Adding core stage targets to 'stage_core_artifacts': ${core_stage_targets}")
  add_dependencies(stage_core_artifacts ${core_stage_targets})
endif()

# The global `stage_all` target depends on the individual staging targets
# from each sub-project. This ensures all artifacts are staged correctly
# before the final message is printed.
add_dependencies(stage_all stage_core_artifacts)

# The `stage_third_party_deps` target (from third_party/CMakeLists.txt) must
# also be executed. To prevent a race condition where both core and third-party
# targets try to create the same directories simultaneously (e.g., in a parallel
# build with `make -j`), we create an explicit dependency. This ensures
# third-party dependencies are staged *before* core artifacts.
if(THIRD_PARTY_INSTALL)
  # This target is defined in third_party/CMakeLists.txt
  add_dependencies(stage_core_artifacts stage_third_party_deps)
endif()

# If the XOP is being built, make sure its staging target is part of the global `stage_all`.
if(BUILD_XOP)
  add_dependencies(stage_all stage_pylabhubxop)
endif()

if(BUILD_TESTS)
  add_dependencies(stage_all stage_tests)
endif()

# --- 3.5. Final Installation and Packaging ---
if(PYLABHUB_CREATE_INSTALL_TARGET)
  # Install the exported targets file.
  install(EXPORT pylabhubTargets
    FILE
      pylabhubTargets.cmake
    NAMESPACE
      pylabhub::
    DESTINATION
      ${CMAKE_INSTALL_LIBDIR}/cmake/pylabhub
  )

  # Generate and install the package version file.
  include(CMakePackageConfigHelpers)
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/pylabhubConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
  )

  # Generate the main package config file.
  configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/pylabhubConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pylabhubConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pylabhub
  )

  # Install the generated config files.
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/pylabhubConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/pylabhubConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pylabhub
  )

  message(STATUS "Installation target enabled. Run 'cmake --install' to create a package.")
else()
  message(STATUS "Installation target disabled.")
endif()

# --- 3.6. Final Configuration Summary ---
message(STATUS "==============================================")
message(STATUS "pyLabHub Configuration Summary")
message(STATUS "----------------------------------------------")

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
message(STATUS "  Utility Library:     pylabhub-utils (SHARED)")
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