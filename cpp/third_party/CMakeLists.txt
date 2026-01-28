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

# --- 2. Define Prerequisite Build Targets ---
# This include defines the 'libsodium_external' target.
include(libsodium)

# This is the master target for all prerequisites.
add_custom_target(build_prerequisites DEPENDS libsodium_external)

# --- 3. Define IMPORTED Targets for Prerequisites ---
# Now that the prerequisite directory structure is known, we can define
# a stable IMPORTED target for libsodium.
set(LIBSODIUM_INCLUDE_DIR "${PREREQ_INSTALL_DIR}/include")
if(MSVC)
  set(LIBSODIUM_LIBRARY_PATH "${PREREQ_INSTALL_DIR}/lib/libsodium.lib")
else()
  set(LIBSODIUM_LIBRARY_PATH "${PREREQ_INSTALL_DIR}/lib/libsodium.a")
endif()

add_library(pylabhub::third_party::sodium STATIC IMPORTED GLOBAL)
set_target_properties(pylabhub::third_party::sodium PROPERTIES
  IMPORTED_LOCATION "${LIBSODIUM_LIBRARY_PATH}"
  INTERFACE_INCLUDE_DIRECTORIES "${LIBSODIUM_INCLUDE_DIR}"
)
# Any target linking to pylabhub::third_party::sodium will now depend on the
# prerequisites being built first.
add_dependencies(pylabhub::third_party::sodium build_prerequisites)


# --- 4. Configure Other Third-Party Libraries ---
# These libraries will now be configured with the stable prerequisite targets available.

# setup nlohmann_json target
include(nlohmann_json)

# setup msgpack target
include(msgpack)

# setup fmt target
include(fmt)

# setup libzmq target
# libzmq's internal targets will depend on 'build_prerequisites',
# the master target for pre-built external dependencies like libsodium.
include(libzmq)

# setup cppzmq target
include(cppzmq)

# setup XOPToolKit
include(XOPToolKit)
include(cmake/googletest.cmake)

# --- 5. Staging ---
# Stage the staged prerequisites
if(THIRD_PARTY_INSTALL)
  pylabhub_register_headers_for_staging(
    DIRECTORIES "${PREREQ_INSTALL_DIR}/include"
    SUBDIR ""
    EXTERNAL_PROJECT_DEPENDENCY libsodium_external
  )
  pylabhub_register_library_for_staging(TARGET pylabhub::third_party::sodium)
endif()

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
