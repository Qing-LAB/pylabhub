# ------------------------------------------------------------------------------
# src/scripting/CMakeLists.txt
#
# Purpose:
#   Configures the `pylabhub-scripting` static library — a thin layer that
#   provides `PythonScriptHost`, the concrete CPython `ScriptHost` subclass.
#
# Rationale for a separate static library:
#   `libpython` (embedded via pybind11::embed) must NOT become a hard dependency
#   of all `pylabhub-utils` consumers.  Pure-C++ clients that never embed Python
#   should link only against `pylabhub-utils`.  Executables that embed Python
#   (pylabhub-hubshell, pylabhub-producer, pylabhub-consumer, pylabhub-processor)
#   link against this additional static lib.
#
# Dependencies:
#   - pylabhub::utils   — ScriptHost base, Logger, platform utilities
#   - pylabhub::third_party::pybind11_embed — pybind11/embed.h + libpython
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

add_library(pylabhub-scripting STATIC
    python_script_host.cpp
    role_host_core.cpp
    python_role_host_base.cpp
    lua_state.cpp
    lua_role_host_base.cpp
)
add_library(pylabhub::scripting ALIAS pylabhub-scripting)

pylabhub_enable_clang_tidy_for_target(pylabhub-scripting)
target_compile_features(pylabhub-scripting PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(pylabhub-scripting PRIVATE /Zc:preprocessor)
endif()

target_compile_definitions(pylabhub-scripting PRIVATE
    LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL}
)

# Private: only the scripting lib's own .cpp needs this directory.
# Consumers find python_script_host.hpp via their own include path addition.
target_include_directories(pylabhub-scripting PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pylabhub-scripting PUBLIC
    pylabhub::utils
    pylabhub::third_party::pybind11_embed
    pylabhub::third_party::luajit
)

pylabhub_apply_sanitizer_to_target(pylabhub-scripting)

message(STATUS "Configured static lib 'pylabhub::scripting' (PythonScriptHost, PythonRoleHostBase, LuaState, LuaRoleHostBase, RoleHostCore).")
