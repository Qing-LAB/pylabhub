# cmake/ToplevelOptions.cmake
#
# Defines the primary, user-facing options for building the pyLabHub project.
# These options control which components are built and how they are configured.
# This module is included by the top-level CMakeLists.txt.

# Option to build the test suite.
option(BUILD_TESTS "Build the pyLabHub test suite" ON)

# Option to build the IgorXOP module.
option(BUILD_XOP "Build the Igor Pro XOP module" ON)

# Define the sanitizer to use.
# Possible values: "None", "Address", "Thread", "UndefinedBehavior", "Undefined".
# Note: AddressSanitizer is supported on MSVC; Thread and UndefinedBehavior sanitizers are not currently supported on MSVC in this build script.
set(PYLABHUB_USE_SANITIZER "None" CACHE STRING "Enable sanitizers (None, Address, Thread, UndefinedBehavior, Undefined). AddressSanitizer supported on MSVC.")
set_property(CACHE PYLABHUB_USE_SANITIZER PROPERTY STRINGS "None" "Address" "Thread" "UndefinedBehavior" "Undefined")
option(PYLABHUB_LINK_STATIC_SANITIZER_INTO_SHARED_LIBS "When using a static sanitizer library (e.g., libasan.a), force it to be linked into the project's shared libraries (like pylabhub-utils). Default is OFF, as it's often better to link the sanitizer into the final executable." OFF)
option(PYLABHUB_SANITIZER_VERBOSE "When sanitizer runtime detection fails, print linker trace and save to build dir for debugging." OFF)

# Option to stage third-party headers and libraries.
option(THIRD_PARTY_INSTALL "Install third-party libraries and headers to the staging directory" ON)

# Option to generate the final 'install' target.
option(PYLABHUB_CREATE_INSTALL_TARGET "Enable the 'install' target to copy the staged directory" ON)

# Option to enforce the use of Apple's clang toolchain on macOS.
option(FORCE_USE_CLANG_ON_APPLE "Force clang on macOS hosts to avoid conflicts with other compilers" ON)

# Option to enable debug logging in the pyLabHub logger.
# When this is turned on, the logger will print to standard output
# the message being logged, and the destination of the log message.
option(PYLABHUB_LOGGER_DEBUG "Enable debug logging in the pyLabHub logger" OFF)

option(PYLABHUB_STAGE_ON_BUILD "Make 'stage_all' run as part of the default build." ON)

# Option to enable Clang-Tidy static analysis.
option(PYLABHUB_ENABLE_CLANG_TIDY "Enable Clang-Tidy static analysis for project targets." OFF)

# Option to control load for racing-condition / stress tests (InProcessSpinState/SpinGuard, SlotRWCoordinator, high_load, FileLock, Logger, JsonConfig, etc.).
# Low: fewer threads, shorter duration (e.g. 10 s). High: more threads, longer duration (e.g. 30 s).
set(STRESS_TEST_LEVEL_VALID "Low" "Medium" "High")
set(STRESS_TEST_LEVEL "Low" CACHE STRING "Stress test level for racing-condition tests: Low (10s, fewer threads), Medium (20s), High (30s, more threads).")
set_property(CACHE STRESS_TEST_LEVEL PROPERTY STRINGS ${STRESS_TEST_LEVEL_VALID})
if(NOT STRESS_TEST_LEVEL IN_LIST STRESS_TEST_LEVEL_VALID)
    message(FATAL_ERROR "Invalid STRESS_TEST_LEVEL: '${STRESS_TEST_LEVEL}'. Must be one of ${STRESS_TEST_LEVEL_VALID}")
endif()

# Option to enable the PLH_DEBUG and PLH_DEBUG_RT macros.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
  option(PYLABHUB_ENABLE_DEBUG_MESSAGES "Enable PLH_DEBUG and PLH_DEBUG_RT messages" ON)
else()
  option(PYLABHUB_ENABLE_DEBUG_MESSAGES "Enable PLH_DEBUG and PLH_DEBUG_RT messages" OFF)
endif()

# --- RecursionGuard max depth ---
# Max recursion depth per thread for RecursionGuard. Exceeding this causes PLH_PANIC.
# Configurable at configure time; passed as compile definition.
set(PLH_RECURSION_GUARD_MAX_DEPTH 64 CACHE STRING "Max recursion depth for RecursionGuard; panic if exceeded (compile-time).")
if(PLH_RECURSION_GUARD_MAX_DEPTH LESS 1 OR PLH_RECURSION_GUARD_MAX_DEPTH GREATER 1024)
  message(FATAL_ERROR "PLH_RECURSION_GUARD_MAX_DEPTH must be between 1 and 1024, got ${PLH_RECURSION_GUARD_MAX_DEPTH}")
endif()

# --- Logger Compile-Time Level ---
# Set the default compile-time log level. This controls which LOGGER_* macros
# are compiled into the binary.
# 0=TRACE, 1=DEBUG, 2=INFO, 3=WARNING, 4=ERROR
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(PYLABHUB_LOGGER_COMPILE_LEVEL 1 CACHE STRING "Default logger compile level") # DEBUG
else()
  set(PYLABHUB_LOGGER_COMPILE_LEVEL 4 CACHE STRING "Default logger compile level") # ERROR for Release, etc.
endif()
message(STATUS "[pylabhub-logger] Logger compile-time level set to: ${PYLABHUB_LOGGER_COMPILE_LEVEL} (0=Trace, 1=Debug, 2=Info, 3=Warn, 4=Error)")