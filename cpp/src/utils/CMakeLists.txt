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
  # Add any new .cpp files for this library to the list above.
)

# Define the shared library target.
add_library(pylabhub-utils SHARED ${UTILS_SOURCES})
add_library(pylabhub::utils ALIAS pylabhub-utils)

# Set modern C++ standard.
target_compile_features(pylabhub-utils PRIVATE cxx_std_17)

# Define PYLABHUB_BUILD_DLL when compiling this library. This is crucial for
# the PYLABHUB_API macro in platform.hpp to use __declspec(dllexport) on Windows.
# Consumers of the library will not have this defined, so they will get __declspec(dllimport).
target_compile_definitions(pylabhub-utils PRIVATE PYLABHUB_BUILD_DLL)

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

# --- Staging ---
# Define a local custom target to handle staging for this component.
pylabhub_get_library_staging_commands(
  TARGET pylabhub-utils
  DESTINATION bin # Shared libraries go to bin/
  OUT_COMMANDS stage_commands
)

add_custom_target(stage_pylabhub_utils ${stage_commands} COMMENT "Staging pylabhub-utils library")
add_dependencies(stage_pylabhub_utils pylabhub-utils)

# Register this local staging target with the global build system.
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_pylabhub_utils)

message(STATUS "Configured shared library 'pylabhub::utils'.")