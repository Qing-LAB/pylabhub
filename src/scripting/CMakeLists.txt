# ------------------------------------------------------------------------------
# src/scripting/CMakeLists.txt
#
# Purpose:
#   Configures the `pylabhub-scripting` static library — the script engine
#   layer providing LuaEngine, PythonEngine, RoleHostCore, and role API classes.
#
# Rationale for a separate static library:
#   `libpython` (embedded via pybind11::embed) must NOT become a hard dependency
#   of all `pylabhub-utils` consumers.  Pure-C++ clients that never embed Python
#   should link only against `pylabhub-utils`.  Executables that embed Python
#   (pylabhub-hubshell, pylabhub-producer, pylabhub-consumer, pylabhub-processor)
#   link against this additional static lib.
#
# Dependencies:
#   - pylabhub::utils   — RoleHostCore, Logger, platform utilities
#   - pylabhub::third_party::pybind11_embed — pybind11/embed.h + libpython
#   - pylabhub::third_party::luajit — LuaJIT for LuaEngine
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

add_library(pylabhub-scripting STATIC
    # engine_module_params.cpp → pylabhub-utils (service/engine_module_params.cpp)
    # native_engine.cpp → pylabhub-utils (service/native_engine.cpp)
    lua_state.cpp
    lua_engine.cpp
    python_engine.cpp
    # Role API classes — in the shared lib so PythonEngine and future
    # dynamic bindings can access all 3 without per-binary compilation.
    ${CMAKE_SOURCE_DIR}/src/producer/producer_api.cpp
    ${CMAKE_SOURCE_DIR}/src/consumer/consumer_api.cpp
    ${CMAKE_SOURCE_DIR}/src/processor/processor_api.cpp
    # Hub-side pybind11 embedded module (HEP-CORE-0033 Phase 7 D4).
    # HubAPI itself is in pylabhub-utils; only the Python binding
    # needs pybind11 deps so it lives here alongside python_engine.
    hub_api_python.cpp
)
add_library(pylabhub::scripting ALIAS pylabhub-scripting)

pylabhub_enable_clang_tidy_for_target(pylabhub-scripting)
target_compile_features(pylabhub-scripting PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(pylabhub-scripting PRIVATE /Zc:preprocessor)
endif()

target_compile_definitions(pylabhub-scripting PRIVATE
    LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL}
    PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH=${PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH}
)

# Include paths: scripting dir + role directories (for API headers).
target_include_directories(pylabhub-scripting PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/producer
    ${CMAKE_SOURCE_DIR}/src/consumer
    ${CMAKE_SOURCE_DIR}/src/processor
)

target_link_libraries(pylabhub-scripting PUBLIC
    pylabhub::utils
    pylabhub::third_party::pybind11_embed
    pylabhub::third_party::luajit
    ${CMAKE_DL_LIBS}
)

pylabhub_apply_sanitizer_to_target(pylabhub-scripting)

message(STATUS "Configured static lib 'pylabhub::scripting' (LuaEngine, PythonEngine, RoleHostCore, role API classes).")
