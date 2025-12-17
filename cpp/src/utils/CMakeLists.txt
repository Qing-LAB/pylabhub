# /cpp/src/utils/CMakeLists.txt
#
# Configures the pylabhub-utils shared library.
# This library contains all common utilities for the project.
#
cmake_minimum_required(VERSION 3.18)

# Define the source files for the utility library explicitly.
# This is more robust than using file(GLOB), as it ensures that adding or
# removing source files requires a direct, version-controlled change to the
# build script. This prevents "silent" build failures where new files are not
# automatically compiled.
set(UTILS_SOURCES
  Logger.cpp
  JsonConfig.cpp
  FileLock.cpp
  AtomicGuard.cpp
  # RecursionGuard.cpp is now a header-only utility.
  # Add any new .cpp files for this library to the list above.
)

# Define the shared library target.
add_library(pylabhub-utils SHARED ${UTILS_SOURCES})
add_library(pylabhub::utils ALIAS pylabhub-utils)

# Set modern C++ standard.
target_compile_features(pylabhub-utils PUBLIC cxx_std_20)

# ==============================================================================
# Symbol Export and Visibility Control
#
# Use GenerateExportHeader to create export macros for this library.
# This ensures that symbols are correctly exported from the DLL on Windows and
# that visibility is managed on other platforms.
include(GenerateExportHeader)
generate_export_header(pylabhub-utils
  BASE_NAME "pylabhub_utils"
  EXPORT_MACRO_NAME "PYLABHUB_UTILS_EXPORT"
)

# By default, we want all symbols to be hidden unless explicitly exported.
# On Windows, this is the default for DLLs.
# On GCC/Clang, we must explicitly set the compiler flag.
if(NOT MSVC)
  target_compile_options(pylabhub-utils PRIVATE -fvisibility=hidden)
endif()

# The generated header needs to be on the include path. Since this is a public
# dependency for consumers of the library, we make it a PUBLIC include.
# The generated header is placed in the build directory for the current source
# directory.
target_include_directories(pylabhub-utils
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
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
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Link against necessary third-party libraries and system libraries.
target_link_libraries(pylabhub-utils
  PUBLIC
    pylabhub::third_party::fmt
    pylabhub::third_party::nlohmann_json
    Threads::Threads
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

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/utils/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/utils
)

# --- Staging ---
# Define a local custom target to handle staging for this component.
pylabhub_get_library_staging_commands(
  TARGET pylabhub-utils
  DESTINATION bin # Shared libraries go to bin/
  OUT_COMMANDS stage_commands
)

add_custom_target(stage_pylabhub_utils ${stage_commands} COMMENT "Staging pylabhub-utils library")
add_dependencies(stage_pylabhub_utils pylabhub-utils)

# Also stage the generated export header alongside the library.
add_custom_command(TARGET stage_pylabhub_utils POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_BINARY_DIR}/pylabhub_utils_export.h"
          "${PYLABHUB_STAGING_DIR}/include/"
  COMMENT "Staging generated export header for pylabhub-utils"
)

# Stage the public headers for this library to match the installation.
add_custom_command(TARGET stage_pylabhub_utils POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/include/utils"
          "${PYLABHUB_STAGING_DIR}/include/utils"
  COMMENT "Staging public headers for pylabhub-utils"
)

# Register this local staging target with the global build system.
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_pylabhub_utils)

message(STATUS "Configured shared library 'pylabhub::utils'.")