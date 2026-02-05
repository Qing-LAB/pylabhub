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

# Validate PYLABHUB_STAGING_DIR. This is crucial as all staging relies on it.
if(NOT DEFINED PYLABHUB_STAGING_DIR OR "${PYLABHUB_STAGING_DIR}" STREQUAL "")
  message(FATAL_ERROR "third_party/CMakeLists.txt expects PYLABHUB_STAGING_DIR to be defined by the parent project.")
endif()

# --- Staging Infrastructure ---
if(THIRD_PARTY_INSTALL)
  add_custom_target(stage_third_party_deps
    COMMENT "Staging third-party dependencies"
  )
  add_dependencies(stage_third_party_deps create_staging_dirs)
  # Ensure all External Projects are built before any staging dependent on them begins.
  add_dependencies(stage_third_party_deps build_prerequisites)
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
# 1. Each prerequisite's .cmake file (e.g., `third_party/cmake/libsodium.cmake`)
#    calls the generic helper function `pylabhub_add_external_prerequisite`.
# 2. This function defines an `ExternalProject_Add` target (e.g., `libsodium_external`)
#    and adds it as a dependency to this `build_prerequisites` target. The helper
#    is generic and handles different build systems (Makefiles, MSBuild, etc.)
#    by accepting custom command lists.
# 3. The helper also creates a stable, namespaced `UNKNOWN IMPORTED` library
#    (e.g., `pylabhub::third_party::libsodium`) that represents the final,
#    normalized artifact.
#
# This ensures that whenever a developer tries to link against a library like
# `pylabhub::third_party::libzmq`, the entire prerequisite build chain is triggered
# automatically and in the correct order.
add_custom_target(build_prerequisites)


# --- 3. Configure All Third-Party Libraries ---
# This section defines how external prerequisites are built and how their
# stable, namespaced targets are created for the rest of the project.

# --- 3.1 External Projects (libsodium, luajit) ---
# These wrappers define the `_external` targets (e.g., `libsodium_external`).
include(libsodium)
include(luajit)

# Hint to subsequent find_package calls where to find the prerequisites we just built.
list(APPEND CMAKE_PREFIX_PATH "${PREREQ_INSTALL_DIR}")

# Manually create the pylabhub::third_party targets for ExternalProject dependencies.
# This explicitly defines the stable interface that our project will link against.

# --- libsodium ---
set(_pkg "libsodium")

# 1. Create a hidden, internal UNKNOWN IMPORTED target for the raw built artifact.
set(_internal_imported_target "_pylabhub_prereq_impl_${_pkg}")
add_library(${_internal_imported_target} UNKNOWN IMPORTED GLOBAL)
set_target_properties(${_internal_imported_target} PROPERTIES
  IMPORTED_LOCATION "${PREREQ_INSTALL_DIR}/lib/${_pkg}-stable${CMAKE_STATIC_LIBRARY_SUFFIX}"
  INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${PREREQ_INSTALL_DIR}/include>;$<INSTALL_INTERFACE:include>"
)
# This target relies on the external project completing its build.
add_dependencies(${_internal_imported_target} libsodium_external)

# 2. Create the canonical INTERFACE wrapper target (pylabhub_libsodium).
add_library(pylabhub_${_pkg} INTERFACE)
target_link_libraries(pylabhub_${_pkg} INTERFACE ${_internal_imported_target})

# 3. Create the public-facing ALIAS (pylabhub::third_party::libsodium).
add_library(pylabhub::third_party::${_pkg} ALIAS pylabhub_${_pkg})


# --- luajit ---
set(_pkg "luajit")

# 1. Create a hidden, internal UNKNOWN IMPORTED target for the raw built artifact.
set(_internal_imported_target "_pylabhub_prereq_impl_${_pkg}")
add_library(${_internal_imported_target} UNKNOWN IMPORTED GLOBAL)
set_target_properties(${_internal_imported_target} PROPERTIES
  IMPORTED_LOCATION "${PREREQ_INSTALL_DIR}/lib/${_pkg}-stable${CMAKE_STATIC_LIBRARY_SUFFIX}"
  INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${PREREQ_INSTALL_DIR}/include>;$<INSTALL_INTERFACE:include>"
)
# This target relies on the external project completing its build.
add_dependencies(${_internal_imported_target} luajit_external)

# 2. Create the canonical INTERFACE wrapper target (pylabhub_luajit).
add_library(pylabhub_${_pkg} INTERFACE)
target_link_libraries(pylabhub_${_pkg} INTERFACE ${_internal_imported_target})

# 3. Create the public-facing ALIAS (pylabhub::third_party::luajit).
add_library(pylabhub::third_party::${_pkg} ALIAS pylabhub_${_pkg})




# --- 3.2 CMake Subprojects (libzmq, fmt, etc.) and Header-Only Libraries ---
# These are included after ExternalProjects and their wrappers are defined.
# They will use standard find_package or add_subdirectory, and create their
# pylabhub::third_party::* targets directly.
include(libzmq) # Now uses add_subdirectory and finds libsodium via CMAKE_PREFIX_PATH
include(nlohmann_json)
include(msgpack)
include(fmt)
include(cppzmq)
include(XOPToolKit)
include(cmake/googletest.cmake)

# --- 4. Staging ---
# The per-target staging registration below only applies to third-party libraries
# that are configured as native CMake sub-projects (e.g., fmt, libzmq).
#
# Prerequisites built via ExternalProject_Add into the 'prereq' directory (e.g., luajit, libsodium)
# are now staged via a separate bulk-copy process defined in the top-level build script.
# We only need to process the final registrations for the remaining targets.


# ===========================================================================
# FINAL PHASE: Process all staging registrations
#
# This must be the very last thing in this file. By this point, all third-party
# libraries have been configured and have registered themselves for staging
# using the helper functions (`pylabhub_register_library_for_staging`,
# `pylabhub_register_headers_for_staging`). These functions append information
# to the global properties `PYLABHUB_LIBRARIES_TO_STAGE` and
# `PYLABHUB_HEADERS_TO_STAGE`. Now, we iterate through those global properties
# to generate the final staging commands.
# ===========================================================================
if(THIRD_PARTY_INSTALL)
  # Use the new holistic registration for external project prerequisites.
  # This replaces the old BulkStagePrerequisites.cmake.in approach.
  pylabhub_register_directory_for_staging(
    SOURCE_DIR "${PREREQ_INSTALL_DIR}"
    ATTACH_TO stage_third_party_deps
    SUBDIRS "bin;lib;include;share" # Stage common directories
  )

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

# ===========================================================================
# FINAL PHASE: Export all public-facing third-party targets for packaging
#
# This must be the very last thing in this file, ensuring all targets are
# fully defined before they are added to the export set.
# ===========================================================================
install(TARGETS
  pylabhub_libsodium
  pylabhub_luajit
  pylabhub_libzmq # This is the INTERFACE target for libzmq
  libzmq-static # Concrete target linked by pylabhub_libzmq, must be exported too.
  pylabhub_fmt # This is the INTERFACE target for fmt
  fmt # Concrete target linked by pylabhub_fmt, must be exported too.
  pylabhub_nlohmann_json # Canonical INTERFACE target for nlohmann_json
  pylabhub_msgpack # Canonical INTERFACE target for msgpack
  pylabhub_cppzmq # Canonical INTERFACE target for cppzmq
  # Add other targets as they are consistently defined
  EXPORT pylabhubTargets
)

