# /cpp/src/utils/CMakeLists.txt
#
# Configures the pylabhub-utils shared library.
# This library contains all common utilities for the project.
#
cmake_minimum_required(VERSION 3.29)

include(MsvcHelper)
include(SanitizerHelper)

message(STATUS "-- start configuring pylabhub::utils dynamic lib...")
# Define the source files for the utility library explicitly.
# This is more robust than using file(GLOB), as it ensures that adding or
# removing source files requires a direct, version-controlled change to the
# build script. This prevents "silent" build failures where new files are not
# automatically compiled.

set(UTILS_SOURCES
  format_tools.cpp
  debug_info.cpp  
  platform.cpp
  Lifecycle.cpp
  FileLock.cpp
  logger_sinks/Sink.cpp
  logger_sinks/BaseFileSink.cpp
  logger_sinks/RotatingFileSink.cpp
  logger_sinks/FileSink.cpp
  logger_sinks/SyslogSink.cpp
  logger_sinks/EventLogSink.cpp
  Logger.cpp
  JsonConfig.cpp
  MessageHub.cpp
  DataBlock.cpp
  shared_memory_mutex.cpp
  shared_spin_lock.cpp
  # Add any new .cpp files for this library to the list above.
)

# Define the shared library target.
add_library(pylabhub-utils SHARED ${UTILS_SOURCES})
add_library(pylabhub::utils ALIAS pylabhub-utils)
# Set modern C++ standard.
target_compile_features(pylabhub-utils PUBLIC cxx_std_20)
if(NOT MSVC)
  target_compile_options(pylabhub-utils PRIVATE -fvisibility=hidden)
endif()

# ------------------------------------------------------------------------------
# Symbol Export and Visibility Control
# ------------------------------------------------------------------------------
include(GenerateExportHeader)

# Use a consistent filename (underscores) and macro name.
# This will generate: <binary-dir>/pylabhub_utils_export.h
# Use an absolute path for EXPORT_FILE_NAME to avoid ambiguity.
generate_export_header(pylabhub-utils
  BASE_NAME         "pylabhub_utils"                 # used for internal checks
  EXPORT_MACRO_NAME "PYLABHUB_UTILS_EXPORT"         # macro used by sources
  EXPORT_FILE_NAME  "${CMAKE_CURRENT_BINARY_DIR}/pylabhub_utils_export.h"  # absolute path
)

# Make sure the generated header directory is on the public include path.
target_include_directories(pylabhub-utils
  PUBLIC
    ${PYLABHUB_INCLUDE_DIR}
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
    $<BUILD_INTERFACE:${PREREQ_INSTALL_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# When building the library itself, the generated header expects a symbol like
# pylabhub_utils_EXPORTS to be defined so the header emits dllexport on Windows.
# GenerateExportHeader creates conditionals based on BASE_NAME/_EXPORTS, so
# define the token the header uses for the build.
# (Do not set PYLABHUB_UTILS_EXPORT_DLL — the generated header uses *_EXPORTS.)
target_compile_definitions(pylabhub-utils PRIVATE pylabhub_utils_EXPORTS)

add_dependencies(pylabhub-utils build_prerequisites)
# ------------------------------------------------------------------------------


# ================================================================================
pylabhub_enable_clang_tidy_for_target(pylabhub-utils)
#=================================================================================

pylabhub_apply_sanitizer_to_target(pylabhub-utils)

# ==============================================================================

pylabhub_apply_standard_msvc_options(pylabhub-utils)

if (PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(pylabhub-utils PRIVATE _LOGGER_DEBUG_ENABLED=1)
    message(STATUS "Building pylabhub-utils with logger debug enabled.")
endif()

# Define the public include directory for consumers of this library.
# This makes `include/` available via `target_include_directories`.


# Link against necessary third-party libraries and system libraries.
target_link_libraries(pylabhub-utils
  PUBLIC
    pylabhub::third_party::fmt
    pylabhub::third_party::nlohmann_json
    pylabhub::third_party::cppzmq
    pylabhub::third_party::libsodium
    pylabhub::third_party::luajit
    Threads::Threads
)

if(PYLABHUB_EXECINFO_LIB)
  target_link_libraries(pylabhub-utils PRIVATE ${PYLABHUB_EXECINFO_LIB})
endif()



# Probe for fmt::chrono subsecond support
include(FmtChronoProbe)
target_compile_definitions(pylabhub-utils
  PRIVATE
    HAVE_FMT_CHRONO_SUBSECONDS=${HAVE_FMT_CHRONO_SUBSECONDS}
    FMT_CHRONO_FMT_STYLE=${FMT_CHRONO_FMT_STYLE}
)



# ==============================================================================
# Installation
#
# Defines the rules for installing this library as part of a package that can
# be used by other CMake projects via `find_package`.
# ==============================================================================
install(TARGETS pylabhub-utils
  EXPORT pylabhubTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Install the public headers for this library.
# The `pylabhub_utils_export.h` header is essential for consumers.
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/pylabhub_utils_export.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/include/utils/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/utils
)

# --- Staging ---
# Define a local custom target to handle staging for this component.
message(STATUS "Configuring staging for pylabhub-utils library.")

if(PYLABHUB_IS_WINDOWS)
  # On Windows, stage the DLL next to the executables and tests
  pylabhub_get_library_staging_commands(TARGET pylabhub-utils DESTINATION bin OUT_COMMANDS stage_commands)
  pylabhub_get_library_staging_commands(TARGET pylabhub-utils DESTINATION tests OUT_COMMANDS stage_commands_for_tests)
  list(APPEND stage_commands ${stage_commands_for_tests})
elseif(PYLABHUB_IS_POSIX)
  # On POSIX platforms, stage the shared library to the 'lib' directory.
  pylabhub_get_library_staging_commands(TARGET pylabhub-utils DESTINATION lib OUT_COMMANDS stage_commands)
else()
  message(FATAL_ERROR "Unknown platform for staging...")
endif()

add_custom_target(stage_pylabhub_utils ${stage_commands} COMMENT "Staging pylabhub-utils library")
add_dependencies(stage_pylabhub_utils pylabhub-utils create_staging_dirs)

# Also stage the generated export header alongside the library.
add_custom_command(TARGET stage_pylabhub_utils POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_BINARY_DIR}/pylabhub_utils_export.h"
          "${PYLABHUB_STAGING_DIR}/include/"
  COMMENT "Staging generated export header for pylabhub-utils"
)

# Register this local staging target with the global build system.
# This ensures that 'stage_pylabhub_utils' will be executed as part of
# the overall `stage_core_artifacts` process.
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_pylabhub_utils)

message(STATUS "-- Configured shared library 'pylabhub::utils'.")
