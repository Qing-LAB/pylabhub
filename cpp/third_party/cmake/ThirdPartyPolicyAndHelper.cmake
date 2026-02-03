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
    set("SNAPSHOT_${_var}_EXISTS" TRUE)
    set("SNAPSHOT_${_var}_VALUE" "${${_var}}")
  else()
    set("SNAPSHOT_${_var}_EXISTS" FALSE)
    set("SNAPSHOT_${_var}_VALUE" "")
  endif()

  # Record cache presence/value (CMake exposes cached value as normal var)
  if(DEFINED CACHE{${_var}})
    set("SNAPSHOT_${_var}_IN_CACHE" TRUE)
    set("SNAPSHOT_${_var}_CACHE_VALUE" "${${_var}}")
  else()
    set("SNAPSHOT_${_var}_IN_CACHE" FALSE)
    set("SNAPSHOT_${_var}_CACHE_VALUE" "")
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

  # Restore cache entry if it existed before
  if(DEFINED SNAPSHOT_${_var}_IN_CACHE AND "${SNAPSHOT_${_var}_IN_CACHE}" STREQUAL "TRUE")
    set(_val "${SNAPSHOT_${_var}_CACHE_VALUE}")
    set(${_var} "${_val}" CACHE ${_type} "restored by restore_cache_var" FORCE)
  else()
    if(DEFINED CACHE{${_var}})
      unset(${_var} CACHE)
    endif()
  endif()

  # Restore normal variable
  if(DEFINED SNAPSHOT_${_var}_EXISTS AND "${SNAPSHOT_${_var}_EXISTS}" STREQUAL "TRUE")
    set(_val "${SNAPSHOT_${_var}_VALUE}")
    set(${_var} "${_val}")
  else()
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
# Expose wrapper: create INTERFACE wrapper and namespaced alias
# _expose_wrapper(WRAPPER_NAME NAMESPACE_ALIAS)
# ----------------------------
function(_expose_wrapper)
  if(ARGC LESS 2)
    message(FATAL_ERROR "_expose_wrapper requires 2 args: wrapper_name namespace_alias")
  endif()
  set(_wrapper "${ARGV0}")
  set(_alias "${ARGV1}")

  if(NOT TARGET ${_wrapper})
    add_library(${_wrapper} INTERFACE)
    # best-effort include dir: prefer PREREQ_INSTALL_DIR if available
    if(DEFINED PREREQ_INSTALL_DIR)
      target_include_directories(${_wrapper} INTERFACE "$<BUILD_INTERFACE:${PREREQ_INSTALL_DIR}/include>" "$<INSTALL_INTERFACE:include>")
    endif()
  endif()

  if(NOT TARGET ${_alias})
    add_library(${_alias} ALIAS ${_wrapper})
  endif()
endfunction()

# ----------------------------
# Primary macro: pylabhub_add_external_prerequisite
# - Registers ExternalProject_<pkg> and creates pylabhub::third_party::<pkg> imported target
# ----------------------------
macro(pylabhub_add_external_prerequisite)
  cmake_parse_arguments(_pylab
    ""
    "NAME;SOURCE_DIR;BINARY_DIR;INSTALL_DIR"
    "DEPENDS;CMAKE_ARGS"
    ${ARGN}
  )

  if(NOT _pylab_NAME)
    message(FATAL_ERROR "pylabhub_add_external_prerequisite requires NAME")
  endif()
  if(NOT _pylab_SOURCE_DIR OR NOT _pylab_BINARY_DIR OR NOT _pylab_INSTALL_DIR)
    message(FATAL_ERROR "pylabhub_add_external_prerequisite requires SOURCE_DIR, BINARY_DIR and INSTALL_DIR")
  endif()

  set(_pkg "${_pylab_NAME}")
  set(_src "${_pylab_SOURCE_DIR}")
  set(_bin "${_pylab_BINARY_DIR}")
  set(_inst "${_pylab_INSTALL_DIR}")

  pylabhub_sanitize_compiler_flags("CMAKE_C_FLAGS" _clean_c_flags)
  pylabhub_sanitize_compiler_flags("CMAKE_CXX_FLAGS" _clean_cxx_flags)

  # Build CMAKE_ARGS list
  set(_cmake_args "")
  if(_pylab_CMAKE_ARGS)
    foreach(_a IN LISTS _pylab_CMAKE_ARGS)
      list(APPEND _cmake_args "${_a}")
    endforeach()
  endif()

  list(FIND _cmake_args "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>" _has_prefix)
  if(_has_prefix EQUAL -1)
    list(APPEND _cmake_args "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>")
  endif()

  list(APPEND _cmake_args "-DCMAKE_C_FLAGS=${_clean_c_flags}")
  list(APPEND _cmake_args "-DCMAKE_CXX_FLAGS=${_clean_cxx_flags}")

  # prepare detect script header
  set(_detect_script "${CMAKE_CURRENT_BINARY_DIR}/detect_${_pkg}.cmake")
  file(WRITE "${_detect_script}" "set(PACKAGE_NAME \"${_pkg}\")\n")
  file(APPEND "${_detect_script}" "set(PREREQ_INSTALL_DIR \"${_inst}\")\n")
  file(APPEND "${_detect_script}" "set(PACKAGE_BINARY_DIR \"${_bin}\")\n")
  if(DEFINED LIB_PATTERNS)
    file(APPEND "${_detect_script}" "set(LIB_PATTERNS \"${LIB_PATTERNS}\")\n")
  else()
    file(APPEND "${_detect_script}" "set(LIB_PATTERNS \"\")\n")
  endif()
  if(DEFINED HEADER_SOURCE_PATTERNS)
    file(APPEND "${_detect_script}" "set(HEADER_SOURCE_PATTERNS \"${HEADER_SOURCE_PATTERNS}\")\n")
  else()
    file(APPEND "${_detect_script}" "set(HEADER_SOURCE_PATTERNS \"\")\n")
  endif()
  file(APPEND "${_detect_script}" "set(STABLE_BASENAME \"${_pkg}-stable\")\n\n")
  file(APPEND "${_detect_script}" "include(\"${CMAKE_CURRENT_LIST_DIR}/detect_external_project.cmake.in\")\n")

  # ensure directories exist
  file(MAKE_DIRECTORY "${_inst}")
  file(MAKE_DIRECTORY "${_inst}/lib")
  file(MAKE_DIRECTORY "${_inst}/include")
  file(MAKE_DIRECTORY "${_bin}")

  set(_stamp "${_inst}/${_pkg}-stamp.txt")

  # prepare ExternalProject args

  #
  # Build an explicit configure command with actual install path substituted.
  # This avoids ExternalProject token substitution ambiguity (so -DCMAKE_INSTALL_PREFIX
  # is always an absolute path instead of a placeholder that may be ignored).
  #
  set(_cmake_args_for_cmd "")
  if(_cmake_args)
    foreach(_ca IN LISTS _cmake_args)
      string(REPLACE "<INSTALL_DIR>" "${_inst}" _ca_subst "${_ca}")
      list(APPEND _cmake_args_for_cmd "${_ca_subst}")
    endforeach()
  endif()

  # Ensure CMAKE_INSTALL_PREFIX is present and absolute.
  list(FIND _cmake_args_for_cmd "-DCMAKE_INSTALL_PREFIX:PATH=${_inst}" _found_prefix2)
  if(_found_prefix2 EQUAL -1)
    list(APPEND _cmake_args_for_cmd "-DCMAKE_INSTALL_PREFIX:PATH=${_inst}")
  endif()

  # Append sanitized flags
  list(APPEND _cmake_args_for_cmd "-DCMAKE_C_FLAGS=${_clean_c_flags}")
  list(APPEND _cmake_args_for_cmd "-DCMAKE_CXX_FLAGS=${_clean_cxx_flags}")

  # Build the configure command list
  set(_configure_cmd_list ${CMAKE_COMMAND} -S "${_src}" -B "${_bin}")
  foreach(_arg IN LISTS _cmake_args_for_cmd)
    list(APPEND _configure_cmd_list "${_arg}")
  endforeach()

  # ExternalProject args: pass the explicit configure command tokens
  set(_ext_args
    SOURCE_DIR "${_src}"
    BINARY_DIR "${_bin}"
    DOWNLOAD_COMMAND ""
  )
  list(APPEND _ext_args CONFIGURE_COMMAND)
  list(APPEND _ext_args ${_configure_cmd_list})
  list(APPEND _ext_args BUILD_COMMAND ${CMAKE_COMMAND} --build "${_bin}" --config ${CMAKE_BUILD_TYPE})
  list(APPEND _ext_args INSTALL_COMMAND ${CMAKE_COMMAND} --build "${_bin}" --target install --config ${CMAKE_BUILD_TYPE})
  list(APPEND _ext_args COMMAND ${CMAKE_COMMAND} -P "${_detect_script}")
  list(APPEND _ext_args BUILD_BYPRODUCTS "${_stamp}")
  


# (NOTE: we intentionally do not append CMAKE_ARGS separately; args are baked
# into the explicit CONFIGURE_COMMAND above to guarantee prefix substitution.)
  

  if(_pylab_DEPENDS)
    foreach(_d IN LISTS _pylab_DEPENDS)
      string(STRIP "${_d}" _d_trim)
      if(_d_trim)
        list(APPEND _ext_args DEPENDS "${_d_trim}_external")
      endif()
    endforeach()
  endif()

  ExternalProject_Add(${_pkg}_external ${_ext_args})

  # create canonical imported target for consumers
  if(${_pkg} MATCHES "^lib")
    set(_stable_lib_base "${_inst}/lib/${_pkg}-stable")
  else()
    set(_stable_lib_base "${_inst}/lib/lib${_pkg}-stable")
  endif()

  if(MSVC)
    set(_stable_lib "${_stable_lib_base}.lib")
  else()
    set(_stable_lib "${_stable_lib_base}.a")
  endif()

  add_library(pylabhub::third_party::${_pkg} UNKNOWN IMPORTED GLOBAL)
  set_target_properties(pylabhub::third_party::${_pkg} PROPERTIES
    IMPORTED_LOCATION "${_stable_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${_inst}/include>;$<INSTALL_INTERFACE:include>"
  )
  add_dependencies(pylabhub::third_party::${_pkg} ${_pkg}_external)
endmacro()

# ----------------------------
# Legacy compatibility utilities
# ----------------------------
# Create a legacy INTERFACE target for header-only packages
function(pylabhub_create_legacy_header_target legacy_name)
  if(NOT TARGET ${legacy_name})
    add_library(${legacy_name} INTERFACE)
    # Use generator expressions so the INTERFACE include dir is valid both in-source and in-build.
    if(DEFINED PREREQ_INSTALL_DIR)
      target_include_directories(${legacy_name} INTERFACE
        "$<BUILD_INTERFACE:${PREREQ_INSTALL_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
      )
    else()
      target_include_directories(${legacy_name} INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/prereqs/include>"
        "$<INSTALL_INTERFACE:include>"
      )
    endif()
  endif()
endfunction()


# Create a legacy UNKNOWN IMPORTED target for libs
function(pylabhub_create_legacy_imported_lib legacy_name pkg_name)
  if(NOT TARGET ${legacy_name})
    add_library(${legacy_name} UNKNOWN IMPORTED GLOBAL)
    if(DEFINED PREREQ_INSTALL_DIR)
      set(_lib_stub "${PREREQ_INSTALL_DIR}/lib/lib${pkg_name}-stable")
      set(_inc_stub "$<BUILD_INTERFACE:${PREREQ_INSTALL_DIR}/include>$<SEMICOLON>$<INSTALL_INTERFACE:include>")
    else()
      set(_lib_stub "${CMAKE_BINARY_DIR}/prereqs/lib/lib${pkg_name}-stable")
      set(_inc_stub "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/prereqs/include>$<SEMICOLON>$<INSTALL_INTERFACE:include>")
    endif()
    set_target_properties(${legacy_name} PROPERTIES
      IMPORTED_LOCATION "${_lib_stub}"
      # INTERFACE_INCLUDE_DIRECTORIES accepts generator expressions; split with SEMICOLON
      INTERFACE_INCLUDE_DIRECTORIES "${_inc_stub}"
    )
  endif()
endfunction()


function(pylabhub_ensure_legacy_alias pkg legacy_name)
  # First consult explicit registrations (highest priority)
  list(FIND THIRD_PARTY_REGISTERED_HEADER_ONLY "${pkg}" _is_hdr_reg)
  list(FIND THIRD_PARTY_REGISTERED_IMPORTED_LIBS "${pkg}" _is_imp_reg)

  if(_is_hdr_reg GREATER -1)
    # registered header-only
    pylabhub_create_legacy_header_target(${legacy_name})
  elseif(_is_imp_reg GREATER -1)
    # registered imported library
    pylabhub_create_legacy_imported_lib(${legacy_name} ${pkg})
  else()
    # fallback: consult the static fallback list
    list(FIND THIRD_PARTY_FALLBACK_HEADER_ONLY "${pkg}" _is_hdr_fallback)
    if(NOT _is_hdr_fallback EQUAL -1)
      pylabhub_create_legacy_header_target(${legacy_name})
    else()
      pylabhub_create_legacy_imported_lib(${legacy_name} ${pkg})
    endif()
  endif()

  # If canonical target exists, prefer creating an ALIAS to it (keeps single source of truth)
  if(TARGET pylabhub::third_party::${pkg} AND NOT TARGET ${legacy_name})
    add_library(${legacy_name} ALIAS pylabhub::third_party::${pkg})
  endif()
endfunction()


# Default legacy aliases used by wrappers; extend if necessary
# pylabhub_ensure_legacy_alias("nlohmann_json" "pylabhub_nlohmann_json")
# pylabhub_ensure_legacy_alias("msgpackc" "pylabhub_msgpackc")
# pylabhub_ensure_legacy_alias("fmt" "pylabhub_fmt")
# Add more mappings below when needed:
# pylabhub_ensure_legacy_alias("libsodium" "pylabhub_libsodium")
# pylabhub_ensure_legacy_alias("libzmq" "pylabhub_libzmq")
# pylabhub_ensure_legacy_alias("luajit" "pylabhub_luajit")

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
