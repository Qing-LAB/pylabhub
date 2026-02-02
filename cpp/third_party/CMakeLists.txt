# third_party/CMakeLists.txt
#
# This file manages and configures external third-party dependencies for the project.
# It sets up prerequisite builds, defines imported targets for these dependencies,
# and orchestrates their staging into the unified staging directory.
#
cmake_minimum_required(VERSION 3.18)
project(third_party NONE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ThirdPartyPolicyAndHelper)

# --- Staging Infrastructure ---
if(THIRD_PARTY_INSTALL)
  add_custom_target(stage_third_party_deps
    COMMENT "Staging third-party dependencies"
  )
  add_dependencies(stage_third_party_deps create_staging_dirs)
endif()

# --- 1. Define Prerequisite Staging Area ---
set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
file(MAKE_DIRECTORY "${PREREQ_INSTALL_DIR}/include")
file(MAKE_DIRECTORY "${PREREQ_INSTALL_DIR}/lib")


# --- 2. Define Prerequisite Build Orchestration ---
# The 'build_prerequisites' custom target serves as the central hub for building
# all external libraries that must be compiled from source before the main project
# can be configured (e.g., libsodium, libzmq).
#
# How it works:
# 1. Each prerequisite's .cmake file (e.g., libsodium.cmake) calls the helper
#    `pylabhub_add_external_prerequisite`, which defines an ExternalProject target
#    (e.g., 'sodium_external') and adds it as a dependency to 'build_prerequisites'.
# 2. The helper also creates the 'pylabhub::third_party::*' IMPORTED library
#    target and makes it depend on 'build_prerequisites'.
#
# This ensures that whenever a developer tries to link against a library like
# 'pylabhub::third_party::libzmq', the entire prerequisite build chain is triggered
# automatically and in the correct order.
add_custom_target(build_prerequisites)


# --- 3. Configure All Third-Party Libraries ---
# The individual scripts will now call the helper function to define the
# `_external` targets and add them to the `build_prerequisites` target.

include(libsodium)
include(luajit)
include(libzmq)

# --- Configure other libraries that might depend on the prerequisites ---
include(nlohmann_json)
include(msgpack)
include(fmt)
include(cppzmq)
include(XOPToolKit)
include(cmake/googletest.cmake)

# --- 4. Staging ---
# This section is now much simpler, as the registration for prerequisites is
# handled inside the `pylabhub_add_external_prerequisite` helper.
# We only need to process the final registrations.


# ===========================================================================
# FINAL PHASE: Process all staging registrations
#
# This must be the very last thing in this file. By this point, all third-party
# libraries have been configured and have registered themselves for staging
# using the helper functions. Now, we process those global registration lists
# to generate the final staging commands.
# ===========================================================================
if(THIRD_PARTY_INSTALL)
    # --- Finalize Third-Party Library Staging ---
    get_property(third_party_libs_to_stage GLOBAL PROPERTY PYLABHUB_LIBRARIES_TO_STAGE)
    if(third_party_libs_to_stage)
        message(STATUS "Processing registered third-party libraries for staging: ${third_party_libs_to_stage}")
        foreach(TGT ${third_party_libs_to_stage})
            if(TARGET ${TGT})
                pylabhub_attach_library_staging_commands(
                    TARGET ${TGT}
                    ATTACH_TO stage_third_party_deps
                )
            else()
                message(WARNING "A registered library target for staging does not exist: ${TGT}")
            endif()
        endforeach()
    endif()

    # --- Finalize Third-Party Header Staging ---
    get_property(header_registrations GLOBAL PROPERTY PYLABHUB_HEADERS_TO_STAGE)
    if(header_registrations)
        message(STATUS "Processing registered third-party headers for staging...")
        foreach(REG_STRING IN LISTS header_registrations)
            if(NOT REG_STRING)
                continue()
            endif()
            
            # The registration string is a set of key-value pairs separated by '@@'.
            # We replace '@@' with ';' to create a standard CMake argument list,
            # and then pass it directly to our helper function.
            string(REPLACE "@@" ";" ARGS_LIST "${REG_STRING}")
            pylabhub_attach_headers_staging_commands(
                ${ARGS_LIST}
                ATTACH_TO stage_third_party_deps
            )
        endforeach()
    endif()
endif()

