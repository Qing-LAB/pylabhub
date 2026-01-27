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
if(PYLABHUB_LOGGER_DEBUG)
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



# --- Staging ---
# Set the output directory for the main executable to place it directly into the
# staged 'bin' directory. Then, register the executable target with the global
# 'stage_core_artifacts' target.
pylabhub_stage_executable(
  TARGET pylabhub-hubshell
  DESTINATION bin
)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS pylabhub-hubshell)



message(STATUS "Configured executable 'pylabhub::hubshell'.")
