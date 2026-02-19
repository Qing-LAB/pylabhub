# ------------------------------------------------------------------------------
# src/broker/CMakeLists.txt
#
# Purpose:
#   Builds the `pylabhub-broker` executable — the central channel discovery
#   service for the DataHub facility. Producers register channels; consumers
#   discover them via REG/DISC/DEREG messages over a ZMQ ROUTER socket.
#
# Dependencies:
#   - pylabhub::utils (Logger, ZMQ headers, nlohmann/json)
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.29)

add_executable(pylabhub-broker
    broker_main.cpp
)

target_compile_features(pylabhub-broker PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(pylabhub-broker PRIVATE /Zc:preprocessor)
endif()

target_compile_definitions(pylabhub-broker PRIVATE
    LOGGER_COMPILE_LEVEL=${PYLABHUB_LOGGER_COMPILE_LEVEL})

if(PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(pylabhub-broker PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()

target_link_libraries(pylabhub-broker PRIVATE pylabhub::utils)

pylabhub_enable_clang_tidy_for_target(pylabhub-broker)
pylabhub_apply_sanitizer_to_target(pylabhub-broker)

# ==============================================================================
# Installation and Staging
# ==============================================================================
install(TARGETS pylabhub-broker DESTINATION ${CMAKE_INSTALL_BINDIR})

pylabhub_stage_executable(TARGET pylabhub-broker DESTINATION bin)
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS pylabhub-broker)

message(STATUS "Configured executable 'pylabhub-broker'.")
