# cpp/src/CMakeLists.txt
#
# Defines the main project targets: pylabhub-corelib and pylabhub-shell.
# This file is included by the top-level CMakeLists.txt.
#
cmake_minimum_required(VERSION 3.18)

# --- 1. pylabhub-corelib ---
# This is the core library containing common utilities.
# Sources are listed explicitly to avoid issues with file(GLOB).
set(CORELIB_SOURCES
  util/Logger.cpp
  fileutil/FileLock.cpp
  fileutil/JsonConfig.cpp
)

if(PYLABHUB_BUILD_SHARED)
  add_library(pylabhub-corelib SHARED ${CORELIB_SOURCES})
else()
  add_library(pylabhub-corelib STATIC ${CORELIB_SOURCES})
endif()

# Expose a stable alias for other project components to use.
add_library(pylabhub::corelib ALIAS pylabhub-corelib)
message(STATUS "Configured target: pylabhub-corelib (alias: pylabhub::corelib)")

# Define public include directories for the core library.
# Note: CMAKE_CURRENT_SOURCE_DIR here is cpp/src, so we go up one level.
target_include_directories(pylabhub-corelib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Link corelib against the third-party targets it needs.
# The third-party framework ensures these targets are always available.
target_link_libraries(pylabhub-corelib
  PUBLIC
    pylabhub::third_party::nlohmann_json # Public because corelib headers may include it.
    pylabhub::third_party::fmt           # Public because static consumers need to link to it.
  PRIVATE
    pylabhub::third_party::zmq
    Threads::Threads
)

# Set standard properties for the core library.
target_compile_features(pylabhub-corelib PUBLIC cxx_std_17)
set_target_properties(pylabhub-corelib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(PYLABHUB_BUILD_SHARED AND MSVC)
  set_target_properties(pylabhub-corelib PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# --- 2. pylabhub-shell (Main Executable) ---
# The logical target name is `pylabhub-shell`.
add_executable(pylabhub-shell coreshell.cpp)
add_executable(pylabhub::shell ALIAS pylabhub-shell)
target_link_libraries(pylabhub-shell PRIVATE pylabhub::corelib)

# Set the output file name to `hubshell` (.exe on Windows).
set_target_properties(pylabhub-shell PROPERTIES OUTPUT_NAME "hubshell")
message(STATUS "Configured target: pylabhub-shell (alias: pylabhub::shell)")


# --- 3. Staging for Main Project Targets ---
# This section defines a dedicated target to stage the core project artifacts
# (executables, libraries, and headers) into the unified staging directory.
# This target is intended to be a dependency of the top-level `stage_all` target,
# ensuring that core artifacts are only staged on demand.

add_custom_target(stage_core_artifacts
  # Stage the main executable to `stage/bin/`.
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/bin"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:pylabhub-shell>" "${PYLABHUB_STAGING_DIR}/bin/"

  # Copy the runtime dependencies (DLLs) of the executable to the bin directory.
  COMMAND ${CMAKE_COMMAND}
    -D TARGET_NAME=pylabhub-shell
    -D DEST_DIR=${PYLABHUB_STAGING_DIR}/bin
    -D "RUNTIME_DEPS=$<TARGET_RUNTIME_DLLS:pylabhub-shell>"
    -P "${CMAKE_SOURCE_DIR}/cmake/StageRuntimeDeps.cmake"

  # Stage the core library file to `stage/lib/`.
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/lib"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:pylabhub-corelib>" "${PYLABHUB_STAGING_DIR}/lib/"

  # Stage the public headers to `stage/include/`.
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/include"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/include/" "${PYLABHUB_STAGING_DIR}/include/"

  COMMENT "Staging hubshell, corelib, and public headers to ${PYLABHUB_STAGING_DIR}"
  VERBATIM
)

# Ensure the artifacts are built before we try to stage them.
add_dependencies(stage_core_artifacts pylabhub-shell pylabhub-corelib)

message(STATUS "Configured target: stage_core_artifacts")
