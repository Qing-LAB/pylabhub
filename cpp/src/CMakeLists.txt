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
  add_library(pylabhub-core INTERFACE)
  target_compile_features(pylabhub-core INTERFACE cxx_std_20)
  set(CORE_LIB_SCOPE INTERFACE)
  message(STATUS "No sources found for pylabhub-core. Creating as an INTERFACE library placeholder.")
else()
  add_library(pylabhub-core STATIC ${CORE_SOURCES})
  target_compile_features(pylabhub-core PRIVATE cxx_std_20)
  set(CORE_LIB_SCOPE PUBLIC)
endif()
add_library(pylabhub::core ALIAS pylabhub-core)

# The core library exposes the main include directory and depends on the utils library
# and other third-party dependencies.
target_include_directories(pylabhub-core
  ${CORE_LIB_SCOPE}
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(pylabhub-core
  ${CORE_LIB_SCOPE}
    pylabhub::utils
    pylabhub::third_party::zmq
)

# Define the main application executable.
add_executable(pylabhub-app hubshell.cpp)
add_executable(pylabhub::app ALIAS pylabhub-app)
target_compile_features(pylabhub-app PRIVATE cxx_std_20)

# Set the compile-time log level based on the build type (Debug/Release).
target_compile_definitions(pylabhub-app PRIVATE LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL})
if(PYLABHUB_LOGGER_DEBUG_ENABLED)
  target_compile_definitions(pylabhub-app PRIVATE _LOGGER_DEBUG_ENABLED=1)
  message(STATUS "Building pylabhub-app with logger debug enabled.")
endif()

# The app links against the core library.
target_link_libraries(pylabhub-app PRIVATE pylabhub::core)

# --- Staging ---

# Create a single local staging target for all artifacts in this file (the app and the core library).
# This target will be registered with the global build system, following the project's modular staging pattern.
# The target is named 'stage_main_artifacts' for clarity, as it stages both the core library and the app.
pylabhub_get_library_staging_commands(
  TARGET pylabhub-core
  DESTINATION lib # Staging destination for the core library
  OUT_COMMANDS stage_core_lib_commands
)
add_custom_target(stage_main_artifacts ${stage_core_lib_commands} COMMENT "Staging main project artifacts (core lib and app)")

# Attach the command to stage the main application executable to this same local target.
pylabhub_stage_executable(
  TARGET pylabhub-app
  DESTINATION bin
  ATTACH_TO stage_main_artifacts
)

# Ensure the local staging target depends on both build targets before executing.
add_dependencies(stage_main_artifacts pylabhub-core pylabhub-app)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_main_artifacts)

message(STATUS "Configured static library 'pylabhub::core' and executable 'pylabhub::app'.")
