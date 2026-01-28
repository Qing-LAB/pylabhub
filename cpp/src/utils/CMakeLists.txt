# /cpp/src/utils/CMakeLists.txt
#
# Configures the pylabhub-utils shared library.
# This library contains all common utilities for the project.
#
cmake_minimum_required(VERSION 3.29)

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
  SharedMemoryHub.cpp
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
    $<INSTALL_INTERFACE:include>
)

# When building the library itself, the generated header expects a symbol like
# pylabhub_utils_EXPORTS to be defined so the header emits dllexport on Windows.
# GenerateExportHeader creates conditionals based on BASE_NAME/_EXPORTS, so
# define the token the header uses for the build.
# (Do not set PYLABHUB_UTILS_EXPORT_DLL — the generated header uses *_EXPORTS.)
target_compile_definitions(pylabhub-utils PRIVATE pylabhub_utils_EXPORTS)

# ------------------------------------------------------------------------------


# ================================================================================
pylabhub_enable_clang_tidy_for_target(pylabhub-utils)
#=================================================================================

if(PYLABHUB_SANITIZER_FLAGS_SET)
  message(STATUS "** Sanitizer detected, adding to pylabhub-utils with compilation flags.")
  target_compile_options(pylabhub-utils PRIVATE $<TARGET_PROPERTY:pylabhub::sanitizer_flags,INTERFACE_COMPILE_OPTIONS>)
  # If sanitizer is enabled, and a runtime path is found, explicitly link to it.
  # This is necessary when a shared library's objects are compiled with sanitizer
  # instrumentation, as the shared library itself needs to resolve these symbols.
  if(PYLABHUB_SANITIZER_RUNTIME_PATH)
    if(PYLABHUB_SANITIZER_RUNTIME_TYPE STREQUAL "shared")
      message(STATUS "** The sanitizer is a dynamic lib, we will link pylabhub-utils to the runtime: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
      target_link_libraries(pylabhub-utils PRIVATE pylabhub::sanitizer_flags)
    else()
      if(PYLABHUB_SANITIZER_RUNTIME_TYPE STREQUAL "static" AND PYLABHUB_SANITIZER_LINK_WITH_DYNAMIC_LIB)
        message(STATUS "** The sanitizer is a static lib, with PYLABHUB_SANITIZER_LINK_WITH_DYNAMIC_LIB=ON, we will force link with pylabhub-utils to: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
        target_link_libraries(pylabhub-utils PRIVATE pylabhub::sanitizer_flags)
      else()
        message(STATUS "** The sanitizer is not a dynamic lib, we will not link by default to pylabhub-utils. Please make sure your final executable that uses pylabhub-utils will be linked to the sanitizer lib: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
        message(STATUS "")
      endif()
    endif()
  endif()
endif()

# ==============================================================================

  # Check if we are using MSVC
if(MSVC)
    # Enable synchronous exception handling and disable warning C5105.
    target_compile_options(pylabhub-utils PRIVATE /EHsc /wd5105)

    # Enable the standards-conforming preprocessor
    target_compile_options(pylabhub-utils PRIVATE /Zc:preprocessor)
endif()

if (PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(pylabhub-utils PRIVATE _LOGGER_DEBUG_ENABLED=1)
    message(STATUS "Building pylabhub-utils with logger debug enabled.")
endif()

# Define the public include directory for consumers of this library.
# This makes `include/` available via `target_include_directories`.
target_include_directories(pylabhub-utils
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

# Link against necessary third-party libraries and system libraries.
target_link_libraries(pylabhub-utils
  PUBLIC
    pylabhub::third_party::fmt
    pylabhub::third_party::nlohmann_json
    pylabhub::third_party::libzmq
    pylabhub::third_party::sodium
    Threads::Threads
)

if(PYLABHUB_EXECINFO_LIB)
  target_link_libraries(pylabhub-utils PRIVATE ${PYLABHUB_EXECINFO_LIB})
endif()

# Ensure libsodium is built before pylabhub-utils (following libzmq pattern)
add_dependencies(pylabhub-utils libsodium_external)

# Probe for fmt::chrono subsecond support
include(FmtChronoProbe)
target_compile_definitions(pylabhub-utils
  PRIVATE
    HAVE_FMT_CHRONO_SUBSECONDS=${HAVE_FMT_CHRONO_SUBSECONDS}
    FMT_CHRONO_FMT_STYLE=${FMT_CHRONO_FMT_STYLE}
)

# Header-only INTERFACE target for consumers that should not link to the shared library.
# It only propagates include dirs, compile definitions and compile options — no linking.
add_library(pylabhub-utils_headers INTERFACE)
add_library(pylabhub::utils_headers ALIAS pylabhub-utils_headers)

# Propagate the same usage requirements that pylabhub-utils publishes for headers.
target_include_directories(pylabhub-utils_headers
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:include>
)

# Propagate any compile definitions the consumers need (but not pylabhub_utils_EXPORTS).
# For consumers we *do not* define pylabhub_utils_EXPORTS (that is only for the library build).
target_compile_definitions(pylabhub-utils_headers
  INTERFACE
    HAVE_FMT_CHRONO_SUBSECONDS=${HAVE_FMT_CHRONO_SUBSECONDS}
    FMT_CHRONO_FMT_STYLE=${FMT_CHRONO_FMT_STYLE}
)

# If you have target_compile_options or features to propagate, add them too:
target_compile_features(pylabhub-utils_headers INTERFACE cxx_std_20)
if(NOT MSVC)
  target_compile_options(pylabhub-utils_headers INTERFACE -fvisibility=hidden)
endif()

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
  message(ERROR, "Unknown platform for staging...")
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
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_pylabhub_utils)

message(STATUS "-- Configured shared library 'pylabhub::utils'.")
