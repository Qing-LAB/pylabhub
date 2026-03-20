# ------------------------------------------------------------------------------
# src/CMakeLists.txt
#
# Purpose:
#   Configures all pyLabHub executables and orchestrates the inclusion of
#   core subdirectories. It establishes build properties, links dependencies,
#   and defines installation and staging rules for all application components.
#
# Responsibilities:
#   - Defines the `pylabhub-hubshell` executable (hub control plane).
#   - Defines the `pylabhub-producer` executable (data producer).
#   - Defines the `pylabhub-consumer` executable (data consumer).
#   - Defines the `pylabhub-processor` executable (data transform processor).
#   - Includes the `utils`, `scripting` subdirectories.
#   - Applies common build settings (C++ standard, Clang-Tidy, sanitizers).
#   - Defines installation rules for all executables.
#   - Configures staging for all executables.
#
# Expected Parent Behavior (from top-level CMakeLists.txt):
#   - Should be included via `add_subdirectory(src)`.
#   - Assumes `PYLABHUB_LOGGER_COMPILE_LEVEL` and `PYLABHUB_LOGGER_DEBUG`
#     are defined.
#   - Assumes `pylabhub_enable_clang_tidy_for_target` and
#     `pylabhub_apply_sanitizer_to_target` functions are available.
#
# Standalone binaries (data plane):
#   - `pylabhub-producer`  — timer-driven data producer  (src/producer/)
#   - `pylabhub-consumer`  — demand-driven data consumer (src/consumer/)
#   - `pylabhub-processor` — transform processor         (src/processor/)
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

# Add the utils subdirectory, which defines the pylabhub::utils library.
# This must be done before defining any targets that depend on it.
add_subdirectory(utils)

# PythonScriptHost static lib (thin CPython embedding layer; keeps libpython
# out of pure-C++ consumers of pylabhub-utils).
add_subdirectory(scripting)

# Standalone data-producer host (embeds CPython + pybind11).
add_subdirectory(producer)

# Standalone data-consumer host (embeds CPython + pybind11).
add_subdirectory(consumer)

# Standalone data-processor host (embeds CPython + pybind11).
add_subdirectory(processor)

# Define the main application executable.
# hub_python/ sources embed the CPython interpreter + pybind11 bindings.
add_executable(pylabhub-hubshell
    hubshell.cpp
    hub_python/python_interpreter.cpp
    hub_python/pylabhub_module.cpp
    hub_python/admin_shell.cpp
    hub_python/hub_script_api.cpp   # HubScriptAPI, ChannelInfo, HubTickInfo + pybind11 module
    hub_python/hub_script.cpp       # HubScript tick thread + script loading
    # PythonScriptHost is used only by HubScript (admin shell).
    # Role binaries use PythonEngine instead. Will be migrated in a future PR.
    scripting/python_script_host.cpp
)
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

# hub_python/ and scripting/ headers are private to the hubshell executable.
target_include_directories(pylabhub-hubshell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hub_python
    ${CMAKE_CURRENT_SOURCE_DIR}/scripting
)

# The app links against the utils library, the PythonScriptHost static lib,
# and embeds CPython via pybind11 (the latter is transitively pulled in by
# pylabhub::scripting, but listed explicitly for clarity).
target_link_libraries(pylabhub-hubshell PRIVATE
    pylabhub::utils
    pylabhub::scripting
    pylabhub::third_party::pybind11_embed
)

pylabhub_apply_sanitizer_to_target(pylabhub-hubshell)

# ==============================================================================
# Installation
# ==============================================================================
# Defines standard installation rules for the executable. This is important for
# CPack integration and allowing downstream projects to find and use this target
# via CMake's package configuration files. This is distinct from the project's
# overall staging mechanism.
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
