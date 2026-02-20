# ------------------------------------------------------------------------------
# src/actor/CMakeLists.txt
#
# Purpose:
#   Configures the `pylabhub-actor` executable — a scripted producer/consumer
#   host that embeds CPython via pybind11 and drives Python callbacks from a
#   C++ event loop.
#
# Responsibilities:
#   - Defines the `pylabhub-actor` executable with its private sources.
#   - Links against pylabhub::utils and the pybind11 embed target.
#   - Applies standard build settings (C++20, clang-tidy, sanitizers).
#   - Stages the executable into bin/ for the staged layout.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.29)

add_executable(pylabhub-actor
    actor_main.cpp
    actor_schema.cpp
    actor_api.cpp
    actor_config.cpp
    actor_module.cpp
    actor_host.cpp
)
add_executable(pylabhub::actor ALIAS pylabhub-actor)

pylabhub_enable_clang_tidy_for_target(pylabhub-actor)
target_compile_features(pylabhub-actor PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(pylabhub-actor PRIVATE /Zc:preprocessor)
endif()

target_compile_definitions(pylabhub-actor PRIVATE
    LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL}
)
if(PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(pylabhub-actor PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()

# actor/ headers are private to this executable.
target_include_directories(pylabhub-actor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pylabhub-actor PRIVATE
    pylabhub::utils
    pylabhub::third_party::pybind11_embed
)

pylabhub_apply_sanitizer_to_target(pylabhub-actor)

# --- Installation ---
install(TARGETS pylabhub-actor DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- Staging ---
pylabhub_stage_executable(
    TARGET pylabhub-actor
    DESTINATION bin
)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS pylabhub-actor)

message(STATUS "Configured executable 'pylabhub::actor'.")
