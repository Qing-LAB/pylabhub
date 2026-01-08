# /cpp/src/CMakeLists.txt
#
# Configures the main project libraries and executables.
#
cmake_minimum_required(VERSION 3.29)

# Add the utils subdirectory, which defines the pylabhub::utils library.
# This must be done before defining any targets that depend on it.
add_subdirectory(utils)

# Gather sources for the core static library.
# This is more robust than using file(GLOB), as it ensures that adding or
# removing source files requires a direct, version-controlled change to the
# build script. This prevents "silent" build failures where new files are not
# automatically compiled.
set(CORE_SOURCES
  "lib/debug_info.cpp"
  "lib/format_tools.cpp"
  "lib/platform.cpp"
)

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
  pylabhub_enable_clang_tidy_for_target(pylabhub-basic)
  target_compile_features(pylabhub-basic PRIVATE cxx_std_20)
  # Check if we are using MSVC
  if(MSVC)
    # Enable the standards-conforming preprocessor
    target_compile_options(pylabhub-basic PRIVATE /Zc:preprocessor)
    target_link_libraries(pylabhub-basic PRIVATE dbghelp.lib)
  else()
    target_link_options(pylabhub-basic PRIVATE -rdynamic)
  endif()
  set(BASIC_LIB_SCOPE PUBLIC)
endif()

if(PYLABHUB_SANITIZER_FLAGS)
  target_link_libraries(pylabhub-basic PRIVATE pylabhub::sanitizer_flags)
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

pylabhub_enable_clang_tidy_for_target(pylabhub-hubshell)
target_compile_features(pylabhub-hubshell PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(pylabhub-hubshell PRIVATE /Zc:preprocessor)
endif()

# Set the compile-time log level based on the build type (Debug/Release).
target_compile_definitions(pylabhub-hubshell PRIVATE LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL})
if(PYLABHUB_LOGGER_DEBUG_ENABLED)
  target_compile_definitions(pylabhub-hubshell PRIVATE _LOGGER_DEBUG_ENABLED=1)
  message(STATUS "Building pylabhub-hubshell with logger debug enabled.")
endif()

# The app links against the core library and the utils library.
target_link_libraries(pylabhub-hubshell PRIVATE pylabhub::basic pylabhub::utils)

if(PYLABHUB_SANITIZER_FLAGS)
  target_link_libraries(pylabhub-hubshell PRIVATE pylabhub::sanitizer_flags)
endif()



# ==============================================================================
# Installation
# ==============================================================================
install(TARGETS pylabhub-hubshell DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS pylabhub-basic
  EXPORT pylabhubTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# This assumes the current CMakeLists is in cpp/src and headers are in cpp/include
get_filename_component(_this_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)   # e.g. .../project/cpp/src
get_filename_component(_cpp_dir "${_this_dir}/.." ABSOLUTE)            # .../project/cpp
set(PYLABHUB_CORE_INCLUDE_DIR "${_cpp_dir}/include")

set(CORE_HEADER_FILES
    "${PYLABHUB_CORE_INCLUDE_DIR}/atomic_guard.hpp"
    "${PYLABHUB_CORE_INCLUDE_DIR}/debug_info.hpp"
    "${PYLABHUB_CORE_INCLUDE_DIR}/format_tools.hpp"
    "${PYLABHUB_CORE_INCLUDE_DIR}/platform.hpp"
    "${PYLABHUB_CORE_INCLUDE_DIR}/recursion_guard.hpp"
    "${PYLABHUB_CORE_INCLUDE_DIR}/scope_guard.hpp"
)

# Install the public headers associated with the core library.
install(FILES ${CORE_HEADER_FILES} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})


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

# Stage the public headers for the core library.
add_custom_command(TARGET stage_main_artifacts POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CORE_HEADER_FILES}
    "${PYLABHUB_STAGING_DIR}/include/"
    COMMENT "Staging core headers"
    VERBATIM
)

# Ensure the local staging target depends on both build targets before executing.
add_dependencies(stage_main_artifacts pylabhub-basic pylabhub-hubshell create_staging_dirs)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_main_artifacts)


message(STATUS "Configured static library 'pylabhub::basic' and executable 'pylabhub::hubshell'.")
