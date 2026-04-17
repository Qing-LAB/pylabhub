cmake_minimum_required(VERSION 3.28)

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

# --- scikit-build-core wheel mode ---
# When driven by scikit-build-core (pip install / pip wheel), SKBUILD is set.
# Force options appropriate for binary wheel packaging: no tests, no XOP,
# no pip install into bundled Python, strip binaries.
if(SKBUILD)
  message(STATUS "scikit-build-core detected — configuring for wheel packaging")
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_XOP OFF CACHE BOOL "" FORCE)
  set(PYLABHUB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(PYLABHUB_STAGE_ON_BUILD ON CACHE BOOL "" FORCE)
  set(PYLABHUB_CREATE_INSTALL_TARGET ON CACHE BOOL "" FORCE)
  set(PYLABHUB_PREPARE_PYTHON_ENV OFF CACHE BOOL "" FORCE)
  set(PYLABHUB_ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
  set(PYLABHUB_ENABLE_DEBUG_MESSAGES OFF CACHE BOOL "" FORCE)
endif()

include(ProjectToolChainSetup)

# ===========================================================================
# PHASE 2: Project Definition and Toolchain Detection
#
# The `project()` command is the most critical part of the configuration.
# It triggers CMake to find and test the compilers for the specified languages.
# After this command, variables like CMAKE_CXX_COMPILER_ID are available.
# ===========================================================================
include(Versions)  # Single source of truth: release, library, python runtime versions

project(pylabhub-cpp
  VERSION "${PYLABHUB_VERSION_MAJOR}.${PYLABHUB_VERSION_MINOR}.${PYLABHUB_VERSION_PATCH}"
  LANGUAGES C CXX
)

# Set the default visibility for symbols to 'hidden'. This is a best practice
# that enables the PYLABHUB_UTILS_EXPORT macro to work correctly on Linux/macOS,
# preventing symbol clashes between shared libraries and the executable.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

include(SanitizeDebugInformation)

# --- Set RPATH for staged/installed executables ---
# This ensures that executables can find shared libraries in a relative path.
# Binaries are in 'bin/', libraries are in 'lib/'. The RPATH should point
# from the executable to the library directory.
# For Linux/macOS, $ORIGIN is a linker directive that resolves to the executable's path.
if(UNIX)
  if(APPLE)
    # On macOS, @loader_path is similar to $ORIGIN but is correctly handled by dyld.
    # We also need to ensure that the executable is marked as allowing rpath.
    # opt/python/lib — standalone Python runtime, always relative to our binary.
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib;@loader_path/../opt/python/lib")
    set(CMAKE_MACOSX_RPATH ON)
  else()
    # opt/python/lib — standalone Python runtime (libpython3.14.so.1.0).
    # Must come before any system path so the dynamic linker never falls through
    # to the system Python installation.
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib:$ORIGIN/../opt/python/lib")
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

# --- 3.1. Global Compile Definitions ---
if(PYLABHUB_ENABLE_DEBUG_MESSAGES)
  add_compile_definitions(PYLABHUB_ENABLE_DEBUG_MESSAGES)
endif()
add_compile_definitions(PLH_RECURSION_GUARD_MAX_DEPTH=${PLH_RECURSION_GUARD_MAX_DEPTH})

# Timeout tier overrides: cmake -DPYLABHUB_SHORT_TIMEOUT_MS=2000 etc.
# See src/include/utils/timeout_constants.hpp for defaults and documentation.
foreach(_timeout_tier SHORT MID LONG)
  set(_timeout_var "PYLABHUB_${_timeout_tier}_TIMEOUT_MS")
  if(DEFINED ${_timeout_var})
    add_compile_definitions(${_timeout_var}=${${_timeout_var}})
    message(STATUS "${_timeout_var} = ${${_timeout_var}}")
  endif()
endforeach()

# --- 3.2. Global Setup and Helper Modules ---
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
message(STATUS "Here is the list of global settings:")
message(STATUS "BUILD_TESTS = ${BUILD_TESTS}")
message(STATUS "BUILD_XOP = ${BUILD_XOP}")
message(STATUS "PYLABHUB_USE_SANITIZER = ${PYLABHUB_USE_SANITIZER}")
message(STATUS "THIRD_PARTY_INSTALL = ${THIRD_PARTY_INSTALL}")
message(STATUS "PYLABHUB_CREATE_INSTALL_TARGET = ${PYLABHUB_CREATE_INSTALL_TARGET}")
message(STATUS "FORCE_USE_CLANG_ON_APPLE = ${FORCE_USE_CLANG_ON_APPLE}")
message(STATUS "PYLABHUB_LOGGER_DEBUG = ${PYLABHUB_LOGGER_DEBUG}")
message(STATUS "PYLABHUB_STAGE_ON_BUILD = ${PYLABHUB_STAGE_ON_BUILD}")
message(STATUS "PYLABHUB_ENABLE_CLANG_TIDY = ${PYLABHUB_ENABLE_CLANG_TIDY}")
message(STATUS "STRESS_TEST_LEVEL = ${STRESS_TEST_LEVEL}")
message(STATUS "PYLABHUB_LOGGER_COMPILE_LEVEL = ${PYLABHUB_LOGGER_COMPILE_LEVEL}")
message(STATUS "PYLABHUB_ENABLE_DEBUG_MESSAGES = ${PYLABHUB_ENABLE_DEBUG_MESSAGES}")
message(STATUS "PLH_RECURSION_GUARD_MAX_DEPTH = ${PLH_RECURSION_GUARD_MAX_DEPTH}")
message(STATUS "PYLABHUB_PYTHON_LOCAL_ARCHIVE = ${PYLABHUB_PYTHON_LOCAL_ARCHIVE}")
message(STATUS "PYLABHUB_PYTHON_WHEELS_DIR = ${PYLABHUB_PYTHON_WHEELS_DIR}")
message(STATUS "PYLABHUB_PREPARE_PYTHON_ENV = ${PYLABHUB_PREPARE_PYTHON_ENV}")
message(STATUS "PYLABHUB_PYTHON_REQUIREMENTS_FILE = ${PYLABHUB_PYTHON_REQUIREMENTS_FILE}")
message(STATUS "CMAKE_INSTALL_PREFIX = ${CMAKE_INSTALL_PREFIX}")
message(STATUS "===============================================================")
message(STATUS "")

# --- 3.3. Staging Infrastructure ---
# This section defines the unified staging directory and the master target
# that drives the entire artifact collection process.

# Determine staging directory name based on CMAKE_BUILD_TYPE.
# For multi-config generators (like VS, Xcode), CMAKE_BUILD_TYPE is typically
# empty at configure time, but available at build time via generator expressions.
# However, if it's set (e.g., from environment or presets), we use it.
# Otherwise, we default to 'debug' for the staging directory name during configure.
if(NOT CMAKE_BUILD_TYPE)
  set(_build_type_suffix "debug") # Default for multi-config generator configure step
else()
  string(TOLOWER "${CMAKE_BUILD_TYPE}" _build_type_suffix)
endif()
set(_staging_dir_name "stage-${_build_type_suffix}")

set(PYLABHUB_STAGING_DIR "${CMAKE_BINARY_DIR}/${_staging_dir_name}")
message(STATUS "Unified staging directory: ${PYLABHUB_STAGING_DIR}")

# Create a dedicated target to initialize all staging directories. This
# prevents race conditions and file/directory name conflicts by ensuring
# the directory structure exists before any artifacts are copied.
add_custom_target(create_staging_dirs
    # Core runtime directories
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/include"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/tests"
    # opt/ — external runtimes (Python, LuaJIT) and interface ports (IgorXOP)
    # Python and LuaJIT are staged as self-contained trees under opt/.
    # Executables find them via relative paths:
    #   RPATH: $ORIGIN/../opt/python/lib (for libpython3.x.so)
    #   C++:   exe_path/../opt/python (PyConfig.home)
    #   C++:   exe_path/../opt/luajit (Lua package.path)
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/opt"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/opt/python"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/opt/IgorXOP"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/opt/luajit"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/opt/luajit/jit"
    # share/ — demos, examples, and utility scripts (populated below)
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/share"
    # config/ — reserved for global configuration files
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/config"
    # docs/ — README and HEP design specifications (populated below)
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/docs"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/docs/HEP"
    COMMENT "Initializing staging directories"
)

# --- Stage Core Project Source Headers Early ---
# This target stages all static source headers that are part of our project,
# before any compilation or library building begins, but after directories are created.
add_custom_target(stage_project_source_headers
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/src/include" "${PYLABHUB_STAGING_DIR}/include"
    COMMENT "Staging project source headers early"
)
add_dependencies(stage_project_source_headers create_staging_dirs)

# --- Stage Documentation ---
# Copy README guides and HEP design specifications so the staged/installed
# package includes user-facing documentation.
add_custom_target(stage_docs
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/docs/README" "${PYLABHUB_STAGING_DIR}/docs"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/docs/HEP" "${PYLABHUB_STAGING_DIR}/docs/HEP"
    COMMENT "Staging documentation (README guides + HEP specifications)"
)
add_dependencies(stage_docs create_staging_dirs)

# --- Stage Share Directory ---
# Copy the entire share/ tree: demos (py-demo-*), examples (py-examples),
# and utility scripts (scripts/python/hubshell_client.py, etc.).
add_custom_target(stage_share
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/share" "${PYLABHUB_STAGING_DIR}/share"
    COMMENT "Staging share directory (demos, examples, scripts)"
)
add_dependencies(stage_share create_staging_dirs)

# --- Stage Python Environment Tools ---
# plh_pyenv: cross-platform tool for managing the bundled Python environment
# (package installation, verification, environment inspection).
# Staged to bin/ alongside the main executables so it's on PATH.
if(PLATFORM_WIN64)
  add_custom_target(stage_pyenv_tools
      COMMAND ${CMAKE_COMMAND} -E copy
              "${CMAKE_SOURCE_DIR}/tools/plh_pyenv.py"
              "${PYLABHUB_STAGING_DIR}/bin/plh_pyenv.py"
      COMMAND ${CMAKE_COMMAND} -E copy
              "${CMAKE_SOURCE_DIR}/tools/plh_pyenv.ps1"
              "${PYLABHUB_STAGING_DIR}/bin/plh_pyenv.ps1"
      COMMENT "Staging plh_pyenv tools to bin/ (Windows)"
      VERBATIM
  )
else()
  add_custom_target(stage_pyenv_tools
      COMMAND ${CMAKE_COMMAND} -E copy
              "${CMAKE_SOURCE_DIR}/tools/plh_pyenv.py"
              "${PYLABHUB_STAGING_DIR}/bin/plh_pyenv.py"
      COMMAND ${CMAKE_COMMAND} -E copy
              "${CMAKE_SOURCE_DIR}/tools/plh_pyenv"
              "${PYLABHUB_STAGING_DIR}/bin/plh_pyenv"
      COMMAND chmod +x "${PYLABHUB_STAGING_DIR}/bin/plh_pyenv"
      COMMENT "Staging plh_pyenv tools to bin/ (Unix)"
      VERBATIM
  )
endif()
add_dependencies(stage_pyenv_tools create_staging_dirs)

# Hub config files (hub.json) are created by `pylabhub-hubshell --init <hub_dir>`.
# They live in user-chosen hub instance directories, not in the staging tree.
# No staging step required — the build system does not manage hub.json files.

# --- Stage Sanitizer Runtimes ---
include(FindSanitizerRuntime)

add_custom_target(stage_all
    COMMAND ${CMAKE_COMMAND} -E echo " "
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------------------------------"
    COMMAND ${CMAKE_COMMAND} -E echo "Staging complete. The project is now ready for installation."
    COMMAND ${CMAKE_COMMAND} -E echo "To install to ${CMAKE_INSTALL_PREFIX}, run: cmake --install ."
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------------------------------"
    COMMAND ${CMAKE_COMMAND} -E touch "${PYLABHUB_STAGING_DIR}/.stage_complete"
    COMMENT "Staging all project dependencies and artifacts to ${PYLABHUB_STAGING_DIR}"
)

if(PYLABHUB_STAGE_ON_BUILD)
  # wrapper that participates in the default ALL target and depends on stage_all
  add_custom_target(stage_all_default ALL
      COMMENT "Wrapper: ensure stage_all runs as part of the default build"
      VERBATIM
  )
  add_dependencies(stage_all_default stage_all)
endif()

# This custom target serves as a hook for all core project artifacts (libraries,
# executables) to attach their staging commands. It is a dependency of `stage_all`.
add_custom_target(stage_core_artifacts)

# Ensure that the core staging process and the overall staging process
# wait for the directories to be created first.
add_dependencies(stage_core_artifacts create_staging_dirs stage_project_source_headers stage_docs stage_share stage_pyenv_tools)

# --- 3.4. Configure All Sub-Projects ---
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

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  message(STATUS "--- Tests Sub-Project Configuration Complete ---")
  message(STATUS "")
else()
  message(STATUS "Skipping tests sub-project (BUILD_TESTS is OFF).")
  message(STATUS "")
endif()

if(PYLABHUB_BUILD_EXAMPLES)
  add_subdirectory(examples)
  message(STATUS "--- Examples Sub-Project Configuration Complete ---")
  message(STATUS "")
endif()

# Configure add-ons, if the directory exists.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/add-ons")
  add_subdirectory(add-ons)
endif()
message(STATUS "--- Optional Sub-Projects Configuration Complete ---")
message(STATUS "")

# --- 3.5. Finalize Staging Dependencies ---
# Sub-projects (like src/utils) register their individual staging targets
# by appending them to the global property `CORE_STAGE_TARGETS`.
# This mechanism allows for a decentralized registration of artifacts
# while centralizing their collection and attachment to the main staging flow.
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
if(BUILD_XOP AND TARGET stage_pylabhub-xop)
  add_dependencies(stage_all stage_pylabhub-xop)
endif()

if(BUILD_TESTS)
  add_dependencies(stage_all stage_tests)
endif()

# --- 3.6. Final Installation and Packaging ---
#
# Installation is a simple copy of the staging directory to the install prefix.
# All artifacts (binaries, libraries, headers, third-party prereqs) are already
# staged with the correct layout during the build. No CMake export/target
# machinery is needed — the staging directory IS the installed layout.
if(PYLABHUB_CREATE_INSTALL_TARGET)
  # Bake the staging directory path into the install-time check script.
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/PreInstallCheck.cmake.in"
    "${CMAKE_BINARY_DIR}/PreInstallCheck.cmake"
    @ONLY
  )
  install(SCRIPT "${CMAKE_BINARY_DIR}/PreInstallCheck.cmake")

  if(SKBUILD)
    # Wheel mode: exclude static archives, test binaries, pkgconfig —
    # only ship runtime artifacts.
    install(
      DIRECTORY "${PYLABHUB_STAGING_DIR}/"
      DESTINATION .
      COMPONENT Runtime
      PATTERN "*.a" EXCLUDE
      PATTERN "*.la" EXCLUDE
      PATTERN "tests" EXCLUDE
      PATTERN "pkgconfig" EXCLUDE
    )
  else()
    install(
      DIRECTORY "${PYLABHUB_STAGING_DIR}/"
      DESTINATION .
      COMPONENT Runtime
    )
  endif()

  message(STATUS "Installation target enabled. Run 'cmake --install' to install staged artifacts.")
else()
  message(STATUS "Installation target disabled.")
endif()

# --- 3.7. Final Configuration Summary ---
message(STATUS "==============================================")
message(STATUS "pyLabHub Configuration Summary")
message(STATUS "----------------------------------------------")

message(STATUS "  Install Prefix:      ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Build Type:          ${CMAKE_BUILD_TYPE}")
message(STATUS "  Utility Library:     pylabhub-utils (SHARED)")
message(STATUS "  Hubshell:            pylabhub-hubshell")
message(STATUS "  Build Tests:         ${BUILD_TESTS}")
message(STATUS "  Build IgorXOP:       ${BUILD_XOP}")
message(STATUS "  3rd-Party Staging:   ${THIRD_PARTY_INSTALL}")
message(STATUS "  Create Install Target: ${PYLABHUB_CREATE_INSTALL_TARGET} (from ${PYLABHUB_STAGING_DIR})")
message(STATUS "==============================================")
message(STATUS "")
message(STATUS "Build configuration is complete.")
message(STATUS "1. To build all targets, run: cmake --build <your_build_directory>")
message(STATUS "2. To stage all artifacts to '${PYLABHUB_STAGING_DIR}', run: cmake --build <your_build_directory> --target stage_all")
message(STATUS "")
