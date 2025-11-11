# --------------------------------------------
# third_party/cmake/nlohmann_json.cmake
# Setup nlohmann/json
#
# Goals:
#  - provide a stable, project-local INTERFACE wrapper target:
#      pylabhub_nlohmann_json
#    and a namespaced alias:
#      pylabhub::nlohmann_json
#
#  - prefer vendor copy when PREFER_VENDOR_NLOHMANN=ON and header exists
#  - prefer upstream exported targets (find_package or FetchContent-provided)
#  - fall back to FetchContent source include dir when necessary
#  - decide whether to install headers/targets using the project's
#    _is_target_installable helper (if provided)
#
# Expectations:
#  - honors higher-level variables:
#      * PREFER_VENDOR_NLOHMANN (defaults OFF)
#      * THIRD_PARTY_INSTALL (controls whether headers are installed)
#  - downstream code should consume pylabhub::nlohmann_json
# --------------------------------------------
message("==================================================================")
message("third_party: Setting up nlohmann/json")
message("==================================================================")

if(NOT DEFINED PREFER_VENDOR_NLOHMANN)
  set(PREFER_VENDOR_NLOHMANN OFF)
endif()

# Local bookkeeping
set(_nlohmann_include_dir "")        # resolved include dir when using header copy (should point to parent 'include' dir)
set(_nlohmann_target_created OFF)    # did we create/link a usable target?
set(_nlohmann_target_name "")        # the 'real' underlying target name (if any)
set(_nlohmann_layout "unknown")      # "namespaced" or "flat" or "unknown" (for messaging)

# Create the project-local wrapper target (concrete name, no ::)
# This is always present and consumers should use pylabhub::nlohmann_json alias.
if(NOT TARGET pylabhub_nlohmann_json)
  add_library(pylabhub_nlohmann_json INTERFACE)
  message(VERBOSE "third_party: created local wrapper pylabhub_nlohmann_json (INTERFACE)")
endif()

#
# Helper: normalize a candidate header path to a canonical include dir.
# Given a header path (e.g. /.../include/nlohmann/json.hpp OR /.../include/json.hpp),
# set _nlohmann_include_dir to the parent "include" directory so that
# -I<include> makes `#include <nlohmann/json.hpp>` valid for the namespaced layout,
# and also supports the flat layout as fallback.
#
function(_resolve_nlohmann_include_dir_from_header _header_path)
  if(NOT EXISTS "${${_header_path}}")
    return()
  endif()

  # get the directory containing the header (e.g. .../include/nlohmann or .../include)
  get_filename_component(_dir "${${_header_path}}" DIRECTORY)

  # basename of that directory (e.g. 'nlohmann' or 'include')
  get_filename_component(_base "${_dir}" NAME)

  if("${_base}" STREQUAL "nlohmann")
    # header path was .../include/nlohmann/json.hpp
    # drop one level so include dir becomes .../include
    get_filename_component(_parent "${_dir}" DIRECTORY)
    set(_nlohmann_include_dir "${_parent}" PARENT_SCOPE)
    set(_nlohmann_layout "namespaced" PARENT_SCOPE)
  else()
    # header path was .../include/json.hpp (flat) or some other layout
    # keep the directory containing the header as include dir (likely .../include)
    set(_nlohmann_include_dir "${_dir}" PARENT_SCOPE)
    set(_nlohmann_layout "flat" PARENT_SCOPE)
  endif()
endfunction()

#
# 1) Vendor copy (explicit vendored headers) — highest preference when requested
#
if(PREFER_VENDOR_NLOHMANN)
  # consider both canonical vendor locations:
  set(_candidate_header1 "${CMAKE_CURRENT_SOURCE_DIR}/include/nlohmann/json.hpp")  # namespaced layout
  set(_candidate_header2 "${CMAKE_CURRENT_SOURCE_DIR}/include/json.hpp")          # flat layout

  if(EXISTS "${_candidate_header1}")
    # namespaced layout found
    set(_tmp_header "${_candidate_header1}")
    _resolve_nlohmann_include_dir_from_header(_tmp_header)
    message(STATUS "third_party: using vendored nlohmann/json headers (namespaced) at ${_nlohmann_include_dir}/nlohmann")
    set(_vendor_header "${_candidate_header1}")

  elseif(EXISTS "${_candidate_header2}")
    # flat layout found
    set(_tmp_header "${_candidate_header2}")
    _resolve_nlohmann_include_dir_from_header(_tmp_header)
    message(STATUS "third_party: using vendored nlohmann/json header (flat) at ${_nlohmann_include_dir}/json.hpp")
    set(_vendor_header "${_candidate_header2}")

  else()
    message(STATUS "third_party: PREFER_VENDOR_NLOHMANN=ON but vendored header not found in ${CMAKE_CURRENT_SOURCE_DIR}/include")
  endif()

  # If we resolved an include dir from vendor, attach it and mark target created
  if(_nlohmann_include_dir AND EXISTS "${_nlohmann_include_dir}")
    target_include_directories(pylabhub_nlohmann_json INTERFACE
      $<BUILD_INTERFACE:${_nlohmann_include_dir}>
      $<INSTALL_INTERFACE:include>    # consumers after 'install' will get <install-root>/include
    )

    set(_nlohmann_target_created ON)
    set(_nlohmann_target_name "pylabhub_nlohmann_json")
    message(STATUS "third_party: pylabhub_nlohmann_json -> vendored headers: ${_nlohmann_include_dir} (layout=${_nlohmann_layout})")
  endif()
endif()

#
# 2) Upstream exported target (preferred if available) or FetchContent fallback
#
if(NOT _nlohmann_target_created)
  include(FetchContent) # safe to include even if find_package was used earlier
  message(STATUS "third_party: attempting to use upstream nlohmann/json or FetchContent fallback")

  # If upstream exported target is present, link to it (do NOT create alias-of-alias).
  if(TARGET nlohmann_json::nlohmann_json)
    message(STATUS "third_party: found upstream target nlohmann_json::nlohmann_json")
    target_link_libraries(pylabhub_nlohmann_json INTERFACE nlohmann_json::nlohmann_json)
    set(_nlohmann_target_created ON)
    set(_nlohmann_target_name "nlohmann_json::nlohmann_json")

  elseif(TARGET nlohmann_json)
    message(STATUS "third_party: found upstream target nlohmann_json")
    target_link_libraries(pylabhub_nlohmann_json INTERFACE nlohmann_json)
    set(_nlohmann_target_created ON)
    set(_nlohmann_target_name "nlohmann_json")

  else()
    # FetchContent fallback — header-only library
    message(STATUS "third_party: FetchContent'ing nlohmann/json (header-only fallback)")
    FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      # Pick a stable tag you trust; update as needed at the project level.
      GIT_TAG v3.11.2
    )
    FetchContent_MakeAvailable(nlohmann_json)

    # Post-FetchContent: prefer any exported target created by the upstream
    if(TARGET nlohmann_json::nlohmann_json)
      message(STATUS "third_party: FetchContent provided target nlohmann_json::nlohmann_json")
      target_link_libraries(pylabhub_nlohmann_json INTERFACE nlohmann_json::nlohmann_json)
      set(_nlohmann_target_created ON)
      set(_nlohmann_target_name "nlohmann_json::nlohmann_json")

    elseif(TARGET nlohmann_json)
      message(STATUS "third_party: FetchContent provided target nlohmann_json")
      target_link_libraries(pylabhub_nlohmann_json INTERFACE nlohmann_json)
      set(_nlohmann_target_created ON)
      set(_nlohmann_target_name "nlohmann_json")

    elseif(DEFINED nlohmann_json_SOURCE_DIR)
      # Try to harvest an include dir from the downloaded source tree
      set(_maybe_include_dir "${nlohmann_json_SOURCE_DIR}/include")

      # normalize fetched tree like vendored logic:
      if(EXISTS "${_maybe_include_dir}/nlohmann/json.hpp")
        set(_nlohmann_include_dir "${_maybe_include_dir}")
        set(_nlohmann_layout "namespaced")
      elseif(EXISTS "${_maybe_include_dir}/json.hpp")
        set(_nlohmann_include_dir "${_maybe_include_dir}")
        set(_nlohmann_layout "flat")
      endif()

      if(_nlohmann_include_dir AND EXISTS "${_nlohmann_include_dir}")
        target_include_directories(pylabhub_nlohmann_json INTERFACE
          $<BUILD_INTERFACE:${_nlohmann_include_dir}>
          $<INSTALL_INTERFACE:include>
        )
        message(STATUS "third_party: configured pylabhub_nlohmann_json from fetched source -> ${_nlohmann_include_dir} (layout=${_nlohmann_layout})")
        set(_nlohmann_target_created ON)
        set(_nlohmann_target_name "pylabhub_nlohmann_json")
      else()
        message(WARNING "third_party: fetched nlohmann_json but headers not found under ${_maybe_include_dir}")
      endif()

    else()
      message(WARNING "third_party: FetchContent made available nlohmann_json but source dir not set")
    endif()
  endif()
endif()

#
# 3) Provide a stable namespaced alias: pylabhub::nlohmann_json
#    Downstream code should consume pylabhub::nlohmann_json and not rely on
#    which real provider was chosen.
#
if(NOT TARGET pylabhub::nlohmann_json)
  add_library(pylabhub::nlohmann_json ALIAS pylabhub_nlohmann_json)
  message(STATUS "third_party: created alias pylabhub::nlohmann_json -> pylabhub_nlohmann_json")
else()
  message(STATUS "third_party: pylabhub::nlohmann_json already exists")
endif()

#
# 4) Export canonical variables to the parent scope for higher-level logic
#
if(_nlohmann_target_created)
  # local variables for messaging
  set(_msg_target "pylabhub::nlohmann_json")
  set(_msg_real "${_nlohmann_target_name}")

  # show messages from current scope
  message(STATUS "third_party: nlohmann/json setup complete")
  message(STATUS "    target NLOHMANN_TARGET = ${_msg_target}")
  message(STATUS "    real target NLOHMANN_REAL_TARGET = ${_msg_real}")

  # then export to parent scope
  set(NLOHMANN_TARGET "${_msg_target}" PARENT_SCOPE)
  set(NLOHMANN_REAL_TARGET "${_msg_real}" PARENT_SCOPE)
else()
  set(NLOHMANN_TARGET "" PARENT_SCOPE)
  set(NLOHMANN_REAL_TARGET "" PARENT_SCOPE)
  message(WARNING "third_party: nlohmann/json setup failed - no usable target or include dir created")
endif()

#
# 5) Install headers (and optionally install target) — only when THIRD_PARTY_INSTALL enabled
#    Use helper _is_target_installable (if present) to decide whether to install the target,
#    and only install headers when we have an include dir.
#
if(DEFINED THIRD_PARTY_INSTALL AND THIRD_PARTY_INSTALL)
  # Decide whether the project considers this wrapper target installable.
  set(_nlohmann_installable OFF)
  _is_target_installable(pylabhub_nlohmann_json _nlohmann_installable)
  message(VERBOSE "third_party: _is_target_installable reported ${_nlohmann_installable} for pylabhub_nlohmann_json")

  # Install headers if we have an include dir
  if(_nlohmann_include_dir AND EXISTS "${_nlohmann_include_dir}")
    # Prefer to install the whole nlohmann/ directory when present; otherwise copy single json.hpp
    if(EXISTS "${_nlohmann_include_dir}/nlohmann")
      install(DIRECTORY "${_nlohmann_include_dir}/nlohmann" DESTINATION include)
      message(STATUS "third_party: nlohmann/json headers scheduled for install from ${_nlohmann_include_dir}/nlohmann -> <install-root>/include/nlohmann")
    elseif(EXISTS "${_nlohmann_include_dir}/json.hpp")
      # Flat fallback — nonstandard: this will put json.hpp directly under <install>/include
      install(FILES "${_nlohmann_include_dir}/json.hpp" DESTINATION include)
      message(STATUS "third_party: nlohmann/json header scheduled for install: ${_nlohmann_include_dir}/json.hpp -> <install-root>/include")
    else()
      message(WARNING "third_party: include dir set (${_nlohmann_include_dir}) but expected header tree not found")
    endif()
  else()
    message(STATUS "third_party: no include dir resolved for nlohmann/json; skipping header install")
  endif()

  # Install CMake export for target if helper indicated target is installable
  if(_nlohmann_installable)
    # Installing an INTERFACE target: use install(TARGETS ... EXPORT ...)
    # This installs the target into project export sets so downstream users can import the target.
    install(TARGETS pylabhub_nlohmann_json
      EXPORT pylabhubThirdPartyTargets
      INCLUDES DESTINATION include
    )

    message(STATUS "third_party: nlohmann/json target pylabhub_nlohmann_json will be installed (export pylabhubThirdPartyTargets)")
  else()
    message(STATUS "third_party: nlohmann/json target not marked installable; skipping target install")
  endif()

  message("")
  message("=========================================================")
  message(STATUS "INSTALL setup nlohmann/json (third-party)")
  if(_nlohmann_include_dir)
    message(STATUS "third_party: nlohmann/json headers will be installed from ${_nlohmann_include_dir}")
  else()
    message(STATUS "third_party: no headers to install for nlohmann/json")
  endif()
  message(STATUS "third_party: nlohmann/json target (alias) = pylabhub::nlohmann_json")
  message("=========================================================")
  message("")
endif()  # THIRD_PARTY_INSTALL
