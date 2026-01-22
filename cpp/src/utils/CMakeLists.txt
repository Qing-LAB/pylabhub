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
  Lifecycle.cpp
  Logger.cpp
  JsonConfig.cpp
  FileLock.cpp
  RotatingFileSink.cpp
  BaseFileSink.cpp
  SharedMemoryHub.cpp
  # Add any new .cpp files for this library to the list above.
)

# Define the shared library target.
add_library(pylabhub-utils SHARED ${UTILS_SOURCES})
add_library(pylabhub::utils ALIAS pylabhub-utils)

pylabhub_enable_clang_tidy_for_target(pylabhub-utils)

# Set modern C++ standard.
target_compile_features(pylabhub-utils PUBLIC cxx_std_20)
target_link_libraries(pylabhub-utils PUBLIC pylabhub-basic)

if(PYLABHUB_SANITIZER_FLAGS_SET)
  message(STATUS "** Sanitizer detected, adding to pylabhub-utils with compilation flags.")
  target_compile_options(pylabhub-utils PRIVATE $<TARGET_PROPERTY:pylabhub::sanitizer_flags,INTERFACE_COMPILE_OPTIONS>)
  # We only apply the compile options for sanitizers to the shared library.
  # The sanitizer runtime should be linked by the final executable, not by the
  # shared library. Linking it here would cause a conflict if the executable
  # also links it, leading to a crash at startup (e.g. "unexpected memory mapping").
  #
  # The following was removed:
  # target_link_libraries(pylabhub-utils PRIVATE pylabhub::sanitizer_flags)
endif()

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

# Ensure the generated header is available to consumers
target_compile_definitions(pylabhub-utils PRIVATE PYLABHUB_UTILS_EXPORT_DLL=1)


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
    pylabhub::third_party::zmq
    pylabhub::third_party::sodium
    Threads::Threads
)

# Ensure libsodium is built before pylabhub-utils (following libzmq pattern)
add_dependencies(pylabhub-utils libsodium_external)

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

if(WIN32)
  # On Windows, stage the DLL next to the executables and tests
  pylabhub_get_library_staging_commands(TARGET pylabhub-utils DESTINATION bin OUT_COMMANDS stage_commands)
  pylabhub_get_library_staging_commands(TARGET pylabhub-utils DESTINATION tests OUT_COMMANDS stage_commands_for_tests)
  list(APPEND stage_commands ${stage_commands_for_tests})
else()
  # On POSIX platforms, stage the shared library to the 'lib' directory.
  pylabhub_get_library_staging_commands(TARGET pylabhub-utils DESTINATION lib OUT_COMMANDS stage_commands)
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

# Stage the public headers for this library to match the installation.
add_custom_command(TARGET stage_pylabhub_utils POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/src/include/utils"
          "${PYLABHUB_STAGING_DIR}/include/utils"
  COMMENT "Staging public headers for pylabhub-utils"
)

# Register this local staging target with the global build system.
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_pylabhub_utils)

message(STATUS "-- Configured shared library 'pylabhub::utils'.")