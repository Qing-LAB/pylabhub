# ------------------------------------------------------------------------------
# src/plh_hub/CMakeLists.txt
#
# Purpose:
#   Configures the `plh_hub` executable — the single-kind hub binary
#   (HEP-CORE-0033 §15).  Replaces the deleted pylabhub-hubshell.
#
# Responsibilities:
#   - Defines the `plh_hub` executable.
#   - Links pylabhub::utils (HubHost, HubConfig, HubDirectory, hub_cli,
#     LifecycleGuard, signal handler) and pylabhub::scripting
#     (engine_factory + concrete engines for script-enabled runs).
#   - Stages to bin/ alongside plh_role.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

add_executable(plh_hub
    plh_hub_main.cpp
)
add_executable(pylabhub::plh_hub ALIAS plh_hub)

pylabhub_enable_clang_tidy_for_target(plh_hub)
target_compile_features(plh_hub PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(plh_hub PRIVATE /Zc:preprocessor)
endif()

target_compile_definitions(plh_hub PRIVATE
    LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL}
)
if(PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(plh_hub PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()

# engine_factory.hpp lives in src/scripting (header-only entry point;
# the library that backs LuaEngine / PythonEngine is pylabhub::scripting).
target_include_directories(plh_hub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripting
)

target_link_libraries(plh_hub PRIVATE
    pylabhub::utils
    pylabhub::scripting
    pylabhub::third_party::pybind11_embed
)

pylabhub_apply_sanitizer_to_target(plh_hub)

# --- Installation ---
install(TARGETS plh_hub DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- Staging ---
pylabhub_stage_executable(
    TARGET plh_hub
    DESTINATION bin
)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS plh_hub)

message(STATUS "Configured executable 'pylabhub::plh_hub'.")
