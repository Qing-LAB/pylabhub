# ------------------------------------------------------------------------------
# src/consumer/CMakeLists.txt
#
# Purpose:
#   Configures the `pylabhub-consumer` executable — a standalone
#   data-consumption host that embeds CPython via pybind11 and drives
#   Python on_consume/on_init/on_stop callbacks from a demand-driven C++ loop.
#
# Responsibilities:
#   - Defines the `pylabhub-consumer` executable with its private sources.
#   - Links against pylabhub::utils and the pybind11 embed target.
#   - Applies standard build settings (C++20, clang-tidy, sanitizers).
#   - Stages the executable into bin/ for the staged layout.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

add_executable(pylabhub-consumer
    consumer_main.cpp
    consumer_role_host.cpp
)
add_executable(pylabhub::consumer ALIAS pylabhub-consumer)

pylabhub_enable_clang_tidy_for_target(pylabhub-consumer)
target_compile_features(pylabhub-consumer PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(pylabhub-consumer PRIVATE /Zc:preprocessor)
endif()

target_compile_definitions(pylabhub-consumer PRIVATE
    LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL}
)
if(PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(pylabhub-consumer PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()

# consumer/ headers are private to this executable.
# ${CMAKE_CURRENT_SOURCE_DIR}/../scripting exposes scripting/python_script_host.hpp
target_include_directories(pylabhub-consumer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripting
)

target_link_libraries(pylabhub-consumer PRIVATE
    pylabhub::utils
    pylabhub::scripting
    pylabhub::third_party::pybind11_embed
)

pylabhub_apply_sanitizer_to_target(pylabhub-consumer)

# --- Installation ---
install(TARGETS pylabhub-consumer DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- Staging ---
pylabhub_stage_executable(
    TARGET pylabhub-consumer
    DESTINATION bin
)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS pylabhub-consumer)

message(STATUS "Configured executable 'pylabhub::consumer'.")
