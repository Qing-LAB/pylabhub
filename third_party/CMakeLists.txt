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

# Create pylabhub::third_party targets for ExternalProject dependencies.
#
# Each ExternalProject prereq gets two targets:
#   pylabhub_<name>                    — INTERFACE (link path + includes + build-order dep)
#   pylabhub::third_party::<name>      — ALIAS (namespaced, for consumers)
#
# pylabhub-utils links these targets as PRIVATE with WHOLE_ARCHIVE
# (see src/utils/CMakeLists.txt) to:
#   1. Embed the static archive symbols directly into pylabhub-utils.so
#   2. Prevent the .a from propagating to downstream executables
# This avoids CI link failures where some toolchains cannot resolve .so
# undefined references from subsequent .a archives on the link line.

# --- libsodium ---
add_library(pylabhub_libsodium INTERFACE)
target_link_libraries(pylabhub_libsodium INTERFACE
  "${PREREQ_INSTALL_DIR}/lib/libsodium-stable${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
target_include_directories(pylabhub_libsodium INTERFACE
  $<BUILD_INTERFACE:${PREREQ_INSTALL_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
add_dependencies(pylabhub_libsodium libsodium_external)
add_library(pylabhub::third_party::libsodium ALIAS pylabhub_libsodium)

# --- luajit ---
add_library(pylabhub_luajit INTERFACE)
target_link_libraries(pylabhub_luajit INTERFACE
  "${PREREQ_INSTALL_DIR}/lib/luajit-stable${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
target_include_directories(pylabhub_luajit INTERFACE
  $<BUILD_INTERFACE:${PREREQ_INSTALL_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
add_dependencies(pylabhub_luajit luajit_external)
add_library(pylabhub::third_party::luajit ALIAS pylabhub_luajit)




# --- 3.2 CMake Subprojects (libzmq, fmt, etc.) and Header-Only Libraries ---
# These are included after ExternalProjects and their wrappers are defined.
# They will use standard find_package or add_subdirectory, and create their
# pylabhub::third_party::* targets directly.
include(libzmq) # Now uses add_subdirectory and finds libsodium via CMAKE_PREFIX_PATH
include(nlohmann_json)
include(msgpack)
include(fmt)
include(cppzmq)
include(python)
include(XOPToolKit)
include(cmake/googletest.cmake)

# --- 4. Staging ---
# The per-target staging registration below only applies to third-party libraries
# that are configured as native CMake sub-projects (e.g., fmt, libzmq).
#
# Prerequisites built via ExternalProject_Add into the 'prereq' directory (e.g., luajit, libsodium)
# are staged via a bulk-copy of the prereqs directory. Note: the static archives (.a) for
# libsodium and luajit archives are embedded in pylabhub-utils.so via WHOLE_ARCHIVE
# (see src/utils/CMakeLists.txt). Only their headers are needed for development.
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
    SUBDIRS "bin;lib;include;share;opt" # Stage common directories (opt/ includes luajit/ and python/)
    CLEAN_RESIDUE # Remove autotools residue (Makefile.in, etc.) before copying
  )

  # --- Finalize Third-Party Library Staging ---
  get_property(third_party_libs_to_stage GLOBAL PROPERTY PYLABHUB_LIBRARIES_TO_STAGE)
  if(third_party_libs_to_stage)
      message(STATUS "Processing registered third-party libraries for staging: ${third_party_libs_to_stage}")
      foreach(ENTRY ${third_party_libs_to_stage})
          string(REPLACE "@@" ";" _parts "${ENTRY}")
          list(GET _parts 0 TGT)
          list(LENGTH _parts _len)
          if(_len GREATER 1)
            list(GET _parts 1 _stable_name)
          else()
            set(_stable_name "")
          endif()
          if(TARGET ${TGT})
              set(_attach_args TARGET ${TGT} ATTACH_TO stage_third_party_deps)
              if(_stable_name)
                list(APPEND _attach_args STABLE_NAME ${_stable_name})
              endif()
              pylabhub_attach_library_staging_commands(${_attach_args})
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
  pylabhub_pybind11_module # Canonical INTERFACE target for Python extension modules
  # Add other targets as they are consistently defined
  EXPORT pylabhubTargets
)

