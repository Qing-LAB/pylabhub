# /cpp/src/CMakeLists.txt
#
# Configures the main project libraries and executables.
#
cmake_minimum_required(VERSION 3.18)

# Add the utils subdirectory, which defines the pylabhub::utils library.
# This must be done before defining any targets that depend on it.
add_subdirectory(utils)

# Gather sources for the core static library.
# Exclude main.cpp (for the app) and sub-project directories.
file(GLOB_RECURSE CORE_SOURCES
  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
  "*.cpp"
)
list(FILTER CORE_SOURCES EXCLUDE REGEX "(hubshell\\.cpp|utils/.*|IgorXOP/.*)$")

# Define the core library. If there are no sources, it will be an INTERFACE
# library that serves as a placeholder to group dependencies. Otherwise, it
# will be a STATIC library.
if(NOT CORE_SOURCES)
  add_library(pylabhub-basic INTERFACE)
  target_compile_features(pylabhub-basic INTERFACE cxx_std_20)
  set(BASIC_LIB_SCOPE INTERFACE)
  message(STATUS "No sources found for pylabhub-basic. Creating as an INTERFACE library placeholder.")
else()
  add_library(pylabhub-basic STATIC ${CORE_SOURCES})
  target_compile_features(pylabhub-basic PRIVATE cxx_std_20)
  # Check if we are using MSVC
  if(MSVC)
    # Enable the standards-conforming preprocessor
    target_compile_options(pylabhub-basic PRIVATE /Zc:preprocessor)
  endif()
  set(BASIC_LIB_SCOPE PUBLIC)
endif()
add_library(pylabhub::basic ALIAS pylabhub-basic)

# The core library exposes the main include directory and depends on the utils library
# and other third-party dependencies.
target_include_directories(pylabhub-basic
  ${BASIC_LIB_SCOPE}
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(pylabhub-basic
  ${BASIC_LIB_SCOPE}
    pylabhub::third_party::zmq
    pylabhub::third_party::fmt
)

# Define the main application executable.
add_executable(pylabhub-hubshell hubshell.cpp)
add_executable(pylabhub::hubshell ALIAS pylabhub-hubshell)
target_compile_features(pylabhub-hubshell PRIVATE cxx_std_20)

# Set the compile-time log level based on the build type (Debug/Release).
target_compile_definitions(pylabhub-hubshell PRIVATE LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL})
if(PYLABHUB_LOGGER_DEBUG_ENABLED)
  target_compile_definitions(pylabhub-hubshell PRIVATE _LOGGER_DEBUG_ENABLED=1)
  message(STATUS "Building pylabhub-hubshell with logger debug enabled.")
endif()

# The app links against the core library.
target_link_libraries(pylabhub-hubshell PRIVATE pylabhub::basic)

# ==============================================================================
# Installation
# ==============================================================================
install(TARGETS pylabhub-hubshell DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS pylabhub-basic
  EXPORT pylabhubTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Install the public headers associated with the core library.
install(FILES
  ${CMAKE_SOURCE_DIR}/include/platform.hpp
  ${CMAKE_SOURCE_DIR}/include/PythonLoader.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# --- Staging ---

# Create a single local staging target for all artifacts in this file (the app and the core library).
# This target will be registered with the global build system, following the project's modular staging pattern.
# The target is named 'stage_main_artifacts' for clarity, as it stages both the core library and the app.
pylabhub_get_library_staging_commands(
  TARGET pylabhub-basic
  DESTINATION lib # Staging destination for the core library
  OUT_COMMANDS stage_core_lib_commands
)
add_custom_target(stage_main_artifacts ${stage_core_lib_commands} COMMENT "Staging main project artifacts (core lib and app)")

# Attach the command to stage the main application executable to this same local target.
pylabhub_stage_executable(
  TARGET pylabhub-hubshell
  DESTINATION bin
  ATTACH_TO stage_main_artifacts
)

# Stage the public headers for the core library to match the installation.
add_custom_command(TARGET stage_main_artifacts POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_SOURCE_DIR}/include/platform.hpp"
          "${CMAKE_SOURCE_DIR}/include/PythonLoader.hpp"
          "${PYLABHUB_STAGING_DIR}/include/"
  COMMENT "Staging public headers for pylabhub-basic"
)

# Ensure the local staging target depends on both build targets before executing.
add_dependencies(stage_main_artifacts pylabhub-basic pylabhub-hubshell create_staging_dirs)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_main_artifacts)

message(STATUS "Configured static library 'pylabhub::basic' and executable 'pylabhub::hubshell'.")
