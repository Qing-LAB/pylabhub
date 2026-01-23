# /cpp/src/CMakeLists.txt
#
# Configures the main project libraries and executables.
#
cmake_minimum_required(VERSION 3.29)

# Add the utils subdirectory, which defines the pylabhub::utils library.
# This must be done before defining any targets that depend on it.
add_subdirectory(utils)

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
target_link_libraries(pylabhub-hubshell PRIVATE pylabhub::utils)

if(PYLABHUB_SANITIZER_FLAGS_SET)
  message(STATUS "** Sanitizer detected, adding to linker flags.")
  target_compile_options(pylabhub-hubshell PRIVATE $<TARGET_PROPERTY:pylabhub::sanitizer_flags,INTERFACE_COMPILE_OPTIONS>)
  target_link_libraries(pylabhub-hubshell PRIVATE pylabhub::sanitizer_flags)
endif()

# ==============================================================================
# Installation
# ==============================================================================
install(TARGETS pylabhub-hubshell DESTINATION ${CMAKE_INSTALL_BINDIR})

# This assumes the current CMakeLists is in cpp/src and headers are in cpp/src/include
get_filename_component(_this_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)   # e.g. .../project/cpp/src
set(PYLABHUB_CORE_INCLUDE_DIR "${_this_dir}/include")

set(CORE_HEADER_FILES
    "${PYLABHUB_CORE_INCLUDE_DIR}/plh_platform.hpp"
    "${PYLABHUB_CORE_INCLUDE_DIR}/plh_base.hpp"
    "${PYLABHUB_CORE_INCLUDE_DIR}/plh_service.hpp"
    "${PYLABHUB_CORE_INCLUDE_DIR}/plh_datahub.hpp"
)

# Install the public headers associated with the core library.
install(FILES ${CORE_HEADER_FILES} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})


# --- Staging ---

# Create a single local staging target for all artifacts in this file (the app and the core library).
# This target will be registered with the global build system, following the project's modular staging pattern.
# The target is named 'stage_main_artifacts' for clarity, as it stages both the core library and the app.
add_custom_target(stage_main_artifacts COMMENT "Staging main project artifacts (core lib and app)")

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
add_dependencies(stage_main_artifacts pylabhub-hubshell create_staging_dirs)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_main_artifacts)


message(STATUS "Configured executable 'pylabhub::hubshell'.")
