# ------------------------------------------------------------------------------
# src/processor/CMakeLists.txt
#
# Purpose:
#   Configures the `pylabhub-processor` executable — a standalone
#   data-transformation host that embeds CPython via pybind11 and drives
#   Python on_process/on_init/on_stop callbacks from a demand-driven C++ loop.
#
# Responsibilities:
#   - Defines the `pylabhub-processor` executable with its private sources.
#   - Links against pylabhub::utils and the pybind11 embed target.
#   - Applies standard build settings (C++20, clang-tidy, sanitizers).
#   - Stages the executable into bin/ for the staged layout.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

add_executable(pylabhub-processor
    processor_main.cpp
    processor_config.cpp
    processor_api.cpp
    processor_script_host.cpp
    lua_processor_host.cpp
    processor_role_host.cpp
)
add_executable(pylabhub::processor ALIAS pylabhub-processor)

pylabhub_enable_clang_tidy_for_target(pylabhub-processor)
target_compile_features(pylabhub-processor PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(pylabhub-processor PRIVATE /Zc:preprocessor)
endif()

target_compile_definitions(pylabhub-processor PRIVATE
    LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL}
)
if(PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(pylabhub-processor PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()

# processor/ headers are private to this executable.
# ${CMAKE_CURRENT_SOURCE_DIR}/../scripting exposes scripting/python_script_host.hpp
target_include_directories(pylabhub-processor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripting
)

target_link_libraries(pylabhub-processor PRIVATE
    pylabhub::utils
    pylabhub::scripting
    pylabhub::third_party::pybind11_embed
)

pylabhub_apply_sanitizer_to_target(pylabhub-processor)

# --- Installation ---
install(TARGETS pylabhub-processor DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- Staging ---
pylabhub_stage_executable(
    TARGET pylabhub-processor
    DESTINATION bin
)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS pylabhub-processor)

message(STATUS "Configured executable 'pylabhub::processor'.")
