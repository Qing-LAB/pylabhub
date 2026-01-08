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
# This is not compatible with MSVC.
set(PYLABHUB_USE_SANITIZER "None" CACHE STRING "Enable sanitizers (None, Address, Thread, UndefinedBehavior, Undefined). Not for MSVC.")
set_property(CACHE PYLABHUB_USE_SANITIZER PROPERTY STRINGS "None" "Address" "Thread" "UndefinedBehavior" "Undefined")

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

# Option to enable the PLH_DEBUG and PLH_DEBUG_RT macros.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
  option(PYLABHUB_ENABLE_DEBUG_MESSAGES "Enable PLH_DEBUG and PLH_DEBUG_RT messages" ON)
else()
  option(PYLABHUB_ENABLE_DEBUG_MESSAGES "Enable PLH_DEBUG and PLH_DEBUG_RT messages" OFF)
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
message(STATUS "Logger compile-time level set to: ${PYLABHUB_LOGGER_COMPILE_LEVEL} (0=Trace, 1=Debug, 2=Info, 3=Warn, 4=Error)")