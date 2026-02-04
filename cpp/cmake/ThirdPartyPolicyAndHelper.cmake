# third_party/cmake/ThirdPartyPolicyAndHelper.cmake
# Consolidated and cleaned helper + policy file for third-party ExternalProject wrappers.
# Place this file before per-package wrapper includes.

cmake_minimum_required(VERSION 3.15)
include(ExternalProject)
include(CMakeParseArguments)

# --- Include guard ---
if(DEFINED THIRD_PARTY_POLICY_AND_HELPER_INCLUDED)
  return()
endif()
set(THIRD_PARTY_POLICY_AND_HELPER_INCLUDED TRUE)

# ----------------------------
# Policy options and defaults
# ----------------------------
option(THIRD_PARTY_ALLOW_UPSTREAM_PCH "Allow upstream projects to enable precompiled headers" OFF)
option(THIRD_PARTY_FORCE_ALLOW_PCH "Force upstream PCH even on MSVC+Ninja (may produce errors)" OFF)
option(THIRD_PARTY_DISABLE_TESTS "Globally disable tests for all third-party libraries." ON)
option(THIRD_PARTY_BUILD_SHARED "Default: build shared third-party libs when supported" OFF)
option(THIRD_PARTY_FORCE_STATIC "Force static builds for third-party libs (overrides shared)" OFF)
option(THIRD_PARTY_VERBOSE_EXTERNAL "Enable verbose external-project build output" OFF)
option(THIRD_PARTY_STRIP_MARCH_FLAGS "Strip -march / -mtune and similar tokens before passing flags to external builds" ON)

# Per-library controls (CACHE so users can override via -D)
set(THIRD_PARTY_FMT_FORCE_VARIANT "static" CACHE STRING "Force fmt build variant: none|shared|static")
set(THIRD_PARTY_ZMQ_FORCE_VARIANT "static" CACHE STRING "Force libzmq build variant: none|shared|static")

option(PYLABHUB_ZMQ_WITH_OPENPGM "Enable OpenPGM transport in libzmq" OFF)
option(PYLABHUB_ZMQ_WITH_NORM "Enable NORM transport in libzmq" OFF)
option(PYLABHUB_ZMQ_WITH_VMCI "Enable VMCI transport in libzmq" OFF)

option(USE_VENDOR_NLOHMANN_ONLY "Force the build to use only the vendored nlohmann/json headers." ON)

# STAGING/INSTALL variables defaults (CACHE)
if(NOT DEFINED PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs" CACHE PATH "Third-party staging install directory")
endif()



#
# Registration API for package semantics (Option B)
# Place this near the top of ThirdPartyPolicyAndHelper.cmake.
#

# Explicit lists (CACHE INTERNAL so other cmake files can append before evaluation if needed)
set(THIRD_PARTY_REGISTERED_HEADER_ONLY "" CACHE INTERNAL "Packages explicitly registered as header-only")
set(THIRD_PARTY_REGISTERED_IMPORTED_LIBS "" CACHE INTERNAL "Packages explicitly registered as binary imported libs")

# Helper to register a package as header-only
macro(pylabhub_register_header_only pkg)
  # append to list if not present
  list(FIND THIRD_PARTY_REGISTERED_HEADER_ONLY "${pkg}" _found)
  if(_found EQUAL -1)
    list(APPEND THIRD_PARTY_REGISTERED_HEADER_ONLY "${pkg}")
    # save back to cache so other included files can read it
    set(THIRD_PARTY_REGISTERED_HEADER_ONLY "${THIRD_PARTY_REGISTERED_HEADER_ONLY}" CACHE INTERNAL "")
  endif()
endmacro()

# Helper to register a package as an imported library (binary)
macro(pylabhub_register_imported_lib pkg)
  list(FIND THIRD_PARTY_REGISTERED_IMPORTED_LIBS "${pkg}" _found)
  if(_found EQUAL -1)
    list(APPEND THIRD_PARTY_REGISTERED_IMPORTED_LIBS "${pkg}")
    set(THIRD_PARTY_REGISTERED_IMPORTED_LIBS "${THIRD_PARTY_REGISTERED_IMPORTED_LIBS}" CACHE INTERNAL "")
  endif()
endmacro()

# ----------------------------
# Utility: sanitize compiler flags
# ----------------------------
function(pylabhub_sanitize_compiler_flags _in_var _out_var)
  set(_val "")
  if(DEFINED ${_in_var})
    set(_val "${${_in_var}}")
  endif()
  if(NOT _val OR NOT THIRD_PARTY_STRIP_MARCH_FLAGS)
    set(${_out_var} "${_val}" PARENT_SCOPE)
    return()
  endif()

  string(REGEX REPLACE "-march=[^ ]+" "" _val "${_val}")
  string(REGEX REPLACE "-mtune=[^ ]+" "" _val "${_val}")
  string(REGEX REPLACE "\\bnocona\\b" "" _val "${_val}")
  string(REGEX REPLACE "-Wl,--as-needed" "" _val "${_val}")
  string(REGEX REPLACE "  +" " " _val "${_val}")
  string(STRIP "${_val}" _val)
  set(${_out_var} "${_val}" PARENT_SCOPE)
endfunction()

# ----------------------------
# Snapshot / restore helpers
# - snapshot_cache_var(VAR): capture existence/value (cache & normal)
# - restore_cache_var(VAR TYPE): restore into CACHE or remove added cache entry
# ----------------------------
function(snapshot_cache_var)
  if(ARGC LESS 1)
    message(FATAL_ERROR "snapshot_cache_var requires at least 1 argument (variable name)")
  endif()
  set(_var "${ARGV0}")

  # Record normal variable existence/value
  if(DEFINED ${_var})
    set(SNAPSHOT_${_var}_EXISTS TRUE)
    set(SNAPSHOT_${_var}_VALUE "${${_var}}")
  else()
    set(SNAPSHOT_${_var}_EXISTS FALSE)
    set(SNAPSHOT_${_var}_VALUE "")
  endif()

  # Record cache presence/value (test cache entry explicitly)
  if(DEFINED CACHE{${_var}})
    set(SNAPSHOT_${_var}_IN_CACHE TRUE)
    # When cache exists, the current value of the variable is the cached value.
    set(SNAPSHOT_${_var}_CACHE_VALUE "${${_var}}")
  else()
    set(SNAPSHOT_${_var}_IN_CACHE FALSE)
    set(SNAPSHOT_${_var}_CACHE_VALUE "")
  endif()
endfunction()

function(restore_cache_var)
  if(ARGC LESS 1)
    message(FATAL_ERROR "restore_cache_var requires at least 1 argument (variable name)")
  endif()
  set(_var "${ARGV0}")
  set(_type "STRING")
  if(ARGC GREATER 1)
    set(_type "${ARGV1}")
  endif()

  # Read snapshot fields into locals to avoid nested expansion pitfalls
  set(_snap_in_cache "${SNAPSHOT_${_var}_IN_CACHE}")
  set(_snap_cache_value "${SNAPSHOT_${_var}_CACHE_VALUE}")
  set(_snap_exists "${SNAPSHOT_${_var}_EXISTS}")
  set(_snap_value "${SNAPSHOT_${_var}_VALUE}")

  # Restore cache entry if it existed before
  if(_snap_in_cache AND "${_snap_in_cache}" STREQUAL "TRUE")
    # restore the cache entry (may be empty string)
    set(${_var} "${_snap_cache_value}" CACHE ${_type} "restored by restore_cache_var" FORCE)
  else()
    # If snapshot said cache did not exist, remove any cache entry that may have been added.
    if(DEFINED CACHE{${_var}})
      unset(${_var} CACHE)
    endif()
  endif()

  # Restore normal variable (non-cache)
  if(_snap_exists AND "${_snap_exists}" STREQUAL "TRUE")
    set(${_var} "${_snap_value}")
  else()
    # remove normal variable if it didn't exist before
    unset(${_var})
  endif()

  # Clean snapshot records
  unset(SNAPSHOT_${_var}_EXISTS)
  unset(SNAPSHOT_${_var}_VALUE)
  unset(SNAPSHOT_${_var}_IN_CACHE)
  unset(SNAPSHOT_${_var}_CACHE_VALUE)
endfunction()


# ----------------------------
# Resolve alias targets recursively to find the concrete target
# _resolve_alias_to_concrete(CANDIDATE OUTVAR)
# ----------------------------
function(_resolve_alias_to_concrete)
  if(ARGC LESS 1)
    message(FATAL_ERROR "_resolve_alias_to_concrete requires at least 1 argument")
  endif()
  set(_candidate "${ARGV0}")
  set(_out_var "")
  if(ARGC GREATER 1)
    set(_out_var "${ARGV1}")
  endif()

  set(_resolved "${_candidate}")
  set(_orig "${_candidate}")
  while(TARGET "${_resolved}")
    get_target_property(_aliased "${_resolved}" ALIASED_TARGET)
    if(_aliased)
      set(_resolved "${_aliased}")
    else()
      break()
    endif()
  endwhile()

  if(_out_var)
    set(${_out_var} "${_resolved}" PARENT_SCOPE)
  else()
    set(_RESOLVED_ALIAS "${_resolved}" PARENT_SCOPE)
  endif()
endfunction()

# ----------------------------
# Helper for printing command lists for debugging ExternalProject calls.
# ----------------------------
function(_pylab_prereq_print_command name command_list_var)
  # command_list_var is the NAME of the list variable, not its content
  set(command_list ${${command_list_var}}) # Dereference the variable name
  list(LENGTH command_list len)
  if(len GREATER 0)
    message(VERBOSE "----------------------------------------------------------------------")
    message(VERBOSE "[pylab_prereq]   - ${name}:")
    foreach(arg IN LISTS command_list)
      # Quote the argument to make spaces visible.
      message(VERBOSE "      - \"${arg}\"")
    endforeach()
  endif()
endfunction()


# ----------------------------
# Primary function: pylabhub_add_external_prerequisite
# - Generic wrapper for ExternalProject_Add that enforces a post-build
#   detection and normalization step.
# - Creates a pylabhub::third_party::<pkg> UNKNOWN IMPORTED target.
# ----------------------------
function(pylabhub_add_external_prerequisite)
  # This function defines a set of standard and custom arguments for the ExternalProject.
  # Standard arguments like NAME, SOURCE_DIR, etc., define the core properties.
  # Custom command arguments (e.g., CONFIGURE_COMMAND) allow overriding default build steps.
  # Pattern arguments (e.g., LIB_PATTERNS) provide inputs for the post-build detection script.
  cmake_parse_arguments(pylab
    ""
    "NAME;SOURCE_DIR;BINARY_DIR;INSTALL_DIR" # Single-value arguments
    "DEPENDS;CMAKE_ARGS;CONFIGURE_COMMAND;BUILD_COMMAND;INSTALL_COMMAND;BUILD_BYPRODUCTS;LIB_PATTERNS;HEADER_SOURCE_PATTERNS" # List-valued arguments
    ${ARGN}
  )

  # --- Argument Validation ---
  if(NOT pylab_NAME)
    message(FATAL_ERROR "pylabhub_add_external_prerequisite requires NAME")
  endif()
  if(NOT pylab_SOURCE_DIR OR NOT pylab_BINARY_DIR OR NOT pylab_INSTALL_DIR)
    message(FATAL_ERROR "pylabhub_add_external_prerequisite requires SOURCE_DIR, BINARY_DIR and INSTALL_DIR")
  endif()

  set(_pkg "${pylab_NAME}")
  set(_src "${pylab_SOURCE_DIR}")
  set(_bin "${pylab_BINARY_DIR}")
  set(_inst "${pylab_INSTALL_DIR}")

  # --- Default CMake Commands ---
  # If no custom commands are provided, fall back to a standard CMake build.
  if(NOT pylab_CONFIGURE_COMMAND)
    pylabhub_sanitize_compiler_flags("CMAKE_C_FLAGS" _clean_c_flags)
    pylabhub_sanitize_compiler_flags("CMAKE_CXX_FLAGS" _clean_cxx_flags)

    set(_default_cmake_args
      "-DCMAKE_INSTALL_PREFIX:PATH=${_inst}"
      "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
      "-DCMAKE_C_FLAGS=${_clean_c_flags}"
      "-DCMAKE_CXX_FLAGS=${_clean_cxx_flags}"
    )
    if(pylab_CMAKE_ARGS)
      list(APPEND _default_cmake_args ${pylab_CMAKE_ARGS})
    endif()
    set(pylab_CONFIGURE_COMMAND ${CMAKE_COMMAND} -S "${_src}" -B "${_bin}" ${_default_cmake_args})
  endif()

  if(NOT pylab_BUILD_COMMAND)
    set(pylab_BUILD_COMMAND ${CMAKE_COMMAND} --build "${_bin}" --config ${CMAKE_BUILD_TYPE})
  endif()

  if(NOT pylab_INSTALL_COMMAND)
    set(pylab_INSTALL_COMMAND ${CMAKE_COMMAND} --build "${_bin}" --target install --config ${CMAKE_BUILD_TYPE})
  endif()

  # --- Debugging Output ---
  message(STATUS "")
  message(STATUS "======================================================================")
  message(STATUS "[pylab_prereq] Configuring external project: ${_pkg}")
  _pylab_prereq_print_command("Configure Command" pylab_CONFIGURE_COMMAND)
  _pylab_prereq_print_command("Build Command"     pylab_BUILD_COMMAND)
  _pylab_prereq_print_command("Install Command"   pylab_INSTALL_COMMAND)
  _pylab_prereq_print_command("Build Byproducts"  pylab_BUILD_BYPRODUCTS)
  _pylab_prereq_print_command("Lib Patterns"      pylab_LIB_PATTERNS)
  _pylab_prereq_print_command("Header Patterns"   pylab_HEADER_SOURCE_PATTERNS)
  _pylab_prereq_print_command("Dependencies"      pylab_DEPENDS)
  message(STATUS "======================================================================")
  message(STATUS "")


  # --- Prepare Post-Build Detection Script ---
  set(_detect_script "${CMAKE_CURRENT_BINARY_DIR}/detect_${_pkg}.cmake")
  file(WRITE  "${_detect_script}" "set(PACKAGE_NAME \"${_pkg}\")\n")
  file(APPEND "${_detect_script}" "set(PREREQ_INSTALL_DIR \"${_inst}\")\n")
  file(APPEND "${_detect_script}" "set(PACKAGE_BINARY_DIR \"${_bin}\")\n")
  file(APPEND "${_detect_script}" "set(LIB_PATTERNS \"${pylab_LIB_PATTERNS}\")\n")
  file(APPEND "${_detect_script}" "set(HEADER_SOURCE_PATTERNS \"${pylab_HEADER_SOURCE_PATTERNS}\")\n")
  file(APPEND "${_detect_script}" "set(STABLE_BASENAME \"${_pkg}-stable\")\n\n")
  # The detection script is located in the third_party/cmake directory.
  file(APPEND "${_detect_script}" "include(\"${CMAKE_SOURCE_DIR}/third_party/cmake/detect_external_project.cmake.in\")\n")

  # --- Ensure directories exist ---


  # --- Construct ExternalProject Arguments ---
  set(_ext_args
    SOURCE_DIR       "${_src}"
    BINARY_DIR       "${_bin}"
    INSTALL_DIR      "${_inst}"
    DOWNLOAD_COMMAND ""
    CONFIGURE_COMMAND ${pylab_CONFIGURE_COMMAND}
    BUILD_COMMAND     ${pylab_BUILD_COMMAND}
    INSTALL_COMMAND   ${pylab_INSTALL_COMMAND}
    COMMAND           ${CMAKE_COMMAND} -P "${_detect_script}" # Post-build normalization
  )

  if(pylab_BUILD_BYPRODUCTS)
    list(APPEND _ext_args BUILD_BYPRODUCTS ${pylab_BUILD_BYPRODUCTS})
  endif()

  if(pylab_DEPENDS)
    set(_ep_deps "")
    foreach(_d IN LISTS pylab_DEPENDS)
      string(STRIP "${_d}" _d_trim)
      if(_d_trim)
        # ExternalProject expects dependencies to have the `_external` suffix.
        list(APPEND _ep_deps "${_d_trim}_external")
      endif()
    endforeach()
    list(APPEND _ext_args DEPENDS ${_ep_deps})
  endif()

  # --- Define the External Project ---
  ExternalProject_Add(${_pkg}_external ${_ext_args})

  # Wire this external project into the master `build_prerequisites` target.
  # This allows a developer to build all prerequisites by building one target.
  add_dependencies(build_prerequisites ${_pkg}_external)

endfunction()

# ----------------------------
# Print brief policy summary (only if configured)
# ----------------------------
message(STATUS "---------------------------------------------------------")
message(STATUS "[pylabhub-third-party] Third-party policy summary:")
if(DEFINED PYLABHUB_CREATE_INSTALL_TARGET)
  message(STATUS "  - Create Install Target: ${PYLABHUB_CREATE_INSTALL_TARGET}")
endif()
message(STATUS "  - Stage 3rd-Party Install Dir: ${PREREQ_INSTALL_DIR}")
message(STATUS "  - Disable tests: ${THIRD_PARTY_DISABLE_TESTS}")
message(STATUS "  - Allow upstream PCH: ${THIRD_PARTY_ALLOW_UPSTREAM_PCH}")
message(STATUS "  - fmt variant: ${THIRD_PARTY_FMT_FORCE_VARIANT}")
message(STATUS "---------------------------------------------------------")
