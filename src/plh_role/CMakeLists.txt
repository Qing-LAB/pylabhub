# ------------------------------------------------------------------------------
# src/plh_role/CMakeLists.txt
#
# Purpose:
#   Configures the `plh_role` executable — the unified role binary that
#   dispatches on the --role CLI flag to construct one of the registered
#   role hosts (producer, consumer, processor) via RoleRegistry.
#
# Replaces pylabhub-producer / pylabhub-consumer / pylabhub-processor once
# Phase 20 (legacy binary deletion) lands.
#
# Responsibilities:
#   - Defines the `plh_role` executable.
#   - Links pylabhub::utils (RoleRegistry, RoleHostBase, RoleDirectory,
#     per-role register_*_init/register_*_runtime symbols) and
#     pylabhub::scripting (engine_factory + concrete engines).
#   - Exposes per-role include dirs so the main can include the three
#     register_*_init.hpp declarations without subdirectory prefixes.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

add_executable(plh_role
    plh_role_main.cpp
)
add_executable(pylabhub::plh_role ALIAS plh_role)

pylabhub_enable_clang_tidy_for_target(plh_role)
target_compile_features(plh_role PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(plh_role PRIVATE /Zc:preprocessor)
endif()

target_compile_definitions(plh_role PRIVATE
    LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL}
)
if(PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(plh_role PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()

# Per-role init headers (register_*_init / register_*_runtime declarations)
# plus the scripting-layer engine_factory.
target_include_directories(plh_role PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../producer
    ${CMAKE_CURRENT_SOURCE_DIR}/../consumer
    ${CMAKE_CURRENT_SOURCE_DIR}/../processor
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripting
)

target_link_libraries(plh_role PRIVATE
    pylabhub::utils
    pylabhub::scripting
    pylabhub::third_party::pybind11_embed
)

pylabhub_apply_sanitizer_to_target(plh_role)

# --- Installation ---
install(TARGETS plh_role DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- Staging ---
pylabhub_stage_executable(
    TARGET plh_role
    DESTINATION bin
)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS plh_role)

message(STATUS "Configured executable 'pylabhub::plh_role'.")
