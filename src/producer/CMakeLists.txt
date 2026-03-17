# ------------------------------------------------------------------------------
# src/producer/CMakeLists.txt
#
# Purpose:
#   Configures the `pylabhub-producer` executable — a standalone
#   data-production host that embeds CPython via pybind11 and drives
#   Python on_produce/on_init/on_stop callbacks from a timer-driven C++ loop.
#
# Responsibilities:
#   - Defines the `pylabhub-producer` executable with its private sources.
#   - Links against pylabhub::utils and the pybind11 embed target.
#   - Applies standard build settings (C++20, clang-tidy, sanitizers).
#   - Stages the executable into bin/ for the staged layout.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

add_executable(pylabhub-producer
    producer_main.cpp
    producer_config.cpp
    producer_api.cpp
    producer_script_host.cpp
    lua_producer_host.cpp
)
add_executable(pylabhub::producer ALIAS pylabhub-producer)

pylabhub_enable_clang_tidy_for_target(pylabhub-producer)
target_compile_features(pylabhub-producer PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(pylabhub-producer PRIVATE /Zc:preprocessor)
endif()

target_compile_definitions(pylabhub-producer PRIVATE
    LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL}
)
if(PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(pylabhub-producer PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()

# producer/ headers are private to this executable.
# ${CMAKE_CURRENT_SOURCE_DIR}/../scripting exposes scripting/python_script_host.hpp
target_include_directories(pylabhub-producer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripting
)

target_link_libraries(pylabhub-producer PRIVATE
    pylabhub::utils
    pylabhub::scripting
    pylabhub::third_party::pybind11_embed
)

pylabhub_apply_sanitizer_to_target(pylabhub-producer)

# --- Installation ---
install(TARGETS pylabhub-producer DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- Staging ---
pylabhub_stage_executable(
    TARGET pylabhub-producer
    DESTINATION bin
)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS pylabhub-producer)

message(STATUS "Configured executable 'pylabhub::producer'.")
