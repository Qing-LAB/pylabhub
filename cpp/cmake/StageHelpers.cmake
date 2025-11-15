# cmake/StageHelpers.cmake
# Helper functions for staging third-party includes/libs and scheduling duplicate checks.
#
# Provides:
#   stage_resolve_include_entry(OUTVAR INC_ENTRY)
#   stage_collect_and_schedule_includes(TARGETS_LIST VENDOR_INCLUDE_DIR STAGE_INCLUDE TARGET_STAGE=<target>)
#   stage_schedule_third_party_libs(TARGETS_LIST STAGE_LIB TARGET_STAGE=<target>)
#   stage_schedule_lib_duplicate_check(STAGE_LIB TARGET_STAGE=<target>)
#
# Implementation notes:
#  - All functions require TARGET_STAGE named arg (the cmake target on which we
#    attach POST_BUILD add_custom_command calls). This is deliberate and safer.
#  - The include-collision check is performed at configure time and will FATAL
#    if the same relative path appears in multiple include roots.
#  - The module is intentionally verbose to help debug why third-party targets
#    may not publish resolvable include directories.
#
cmake_minimum_required(VERSION 3.18)

# ---------------------------------------------------------------------------
# Small helper: robustly resolve a "TARGET_STAGE" named argument from ARGN.
# ---------------------------------------------------------------------------
function(_stage_helpers__resolve_target_stage out_var)
  set(_ts "stage_install")
  if(ARGC GREATER 0)
    foreach(_a IN LISTS ARGN)
      if(_a MATCHES "^TARGET_STAGE=")
        string(REGEX REPLACE "^TARGET_STAGE=(.*)$" "\\1" _ts "${_a}")
      endif()
    endforeach()
  endif()
  set(${out_var} "${_ts}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# stage_resolve_include_entry: resolve generator-expr BUILD_INTERFACE or relative
# returns absolute path when it exists, otherwise best-effort string.
# ---------------------------------------------------------------------------
function(stage_resolve_include_entry OUTVAR INC_ENTRY)
  if(NOT INC_ENTRY)
    set(${OUTVAR} "" PARENT_SCOPE)
    return()
  endif()

  message(STATUS "StageHelpers: resolving include entry raw='${INC_ENTRY}'")
  set(_resolved "")

  # handle $<BUILD_INTERFACE:...> patterns
  if("${INC_ENTRY}" MATCHES "\\$<BUILD_INTERFACE:([^>]+)>")
    string(REGEX REPLACE ".*\\$<BUILD_INTERFACE:([^>]+)>.*" "\\1" _maybe "${INC_ENTRY}")
    message(STATUS "StageHelpers:  BUILD_INTERFACE content -> '${_maybe}'")
    if(EXISTS "${CMAKE_BINARY_DIR}/${_maybe}")
      file(REAL_PATH "${CMAKE_BINARY_DIR}/${_maybe}" _resolved_abs)
      set(_resolved "${_resolved_abs}")
      message(STATUS "StageHelpers:  resolved -> ${_resolved} (binary dir)")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/${_maybe}")
      file(REAL_PATH "${CMAKE_SOURCE_DIR}/${_maybe}" _resolved_abs)
      set(_resolved "${_resolved_abs}")
      message(STATUS "StageHelpers:  resolved -> ${_resolved} (source dir)")
    else()
      set(_resolved "${_maybe}")
      message(STATUS "StageHelpers:  BUILD_INTERFACE path not present; keeping '${_maybe}'")
    endif()
  else()
    # not a generator expr: resolve absolute or try source/binary relative
    if(IS_ABSOLUTE "${INC_ENTRY}")
      set(_resolved "${INC_ENTRY}")
      message(STATUS "StageHelpers:  absolute path -> ${_resolved}")
    else()
      if(EXISTS "${CMAKE_SOURCE_DIR}/${INC_ENTRY}")
        file(REAL_PATH "${CMAKE_SOURCE_DIR}/${INC_ENTRY}" _resolved_abs)
        set(_resolved "${_resolved_abs}")
        message(STATUS "StageHelpers:  resolved relative -> source/${INC_ENTRY} -> ${_resolved}")
      elseif(EXISTS "${CMAKE_BINARY_DIR}/${INC_ENTRY}")
        file(REAL_PATH "${CMAKE_BINARY_DIR}/${INC_ENTRY}" _resolved_abs)
        set(_resolved "${_resolved_abs}")
        message(STATUS "StageHelpers:  resolved relative -> binary/${INC_ENTRY} -> ${_resolved}")
      else()
        set(_resolved "${INC_ENTRY}")
        message(STATUS "StageHelpers:  relative path not found; keeping raw -> '${_resolved}'")
      endif()
    endif()
  endif()

  if(EXISTS "${_resolved}")
    file(REAL_PATH "${_resolved}" _resolved_abs)
    set(${OUTVAR} "${_resolved_abs}" PARENT_SCOPE)
  else()
    set(${OUTVAR} "${_resolved}" PARENT_SCOPE)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# stage_collect_and_schedule_includes(TARGETS_LIST VENDOR_INCLUDE_DIR STAGE_INCLUDE TARGET_STAGE=...)
#
# TARGETS_LIST may be:
#   - the name of a variable (e.g. _THIRD_PARTY_TARGETS_TO_STAGE) which holds
#     a semicolon-separated list; OR
#   - a literal semicolon-separated list passed inline.
#
# VENDOR_INCLUDE_DIR is an optional conventional vendor include root.
# STAGE_INCLUDE is the destination directory under staging.
#
# Requires named arg: TARGET_STAGE=<cmake-target>
# ---------------------------------------------------------------------------
function(stage_collect_and_schedule_includes TARGETS_LIST VENDOR_INCLUDE_DIR STAGE_INCLUDE)
  message(STATUS "******** stage_collect_and_schedule_includes arguments:")
  # Parse named args robustly (use quoted ARGN)
  cmake_parse_arguments(SH "" "" "TARGET_STAGE" ${ARGN})
  message(STATUS "         Arugments received: TARGETS_LIST: ${${TARGETS_LIST}}")
  message(STATUS "         Arugments received: VENDOR_INCLUDE_DIR: ${VENDOR_INCLUDE_DIR}")
  message(STATUS "         Arugments received: STAGE_INCLUDE: ${STAGE_INCLUDE}")
  message(STATUS "         Arugments received: TARGET_STAGE: ${SH_TARGET_STAGE}")
  set(_target_stage "${SH_TARGET_STAGE}")
  if(NOT _target_stage)
    message(FATAL_ERROR "stage_collect_and_schedule_includes: must provide TARGET_STAGE=<cmake-target>")
  endif()
  message(STATUS "StageHelpers: using TARGET_STAGE='${_target_stage}'")

  # Expand targets list: allow passing variable-name or inline list
  set(_targets_val "")
  if(DEFINED ${TARGETS_LIST} AND NOT "${TARGETS_LIST}" STREQUAL "")
    # treat TARGETS_LIST as variable name holding the list
    set(_targets_val "${${TARGETS_LIST}}")
    message(STATUS "StageHelpers: TARGETS_LIST resolved from variable '${TARGETS_LIST}' -> '${_targets_val}'")
  else()
    # treat the argument itself as literal list
    set(_targets_val "${TARGETS_LIST}")
    message(STATUS "StageHelpers: TARGETS_LIST treated as literal -> '${_targets_val}'")
  endif()

  set(_resolved_includes "")

  foreach(_t IN LISTS _targets_val)
    if(NOT _t)
      continue()
    endif()
    if(TARGET "${_t}")
      message(STATUS "StageHelpers: probing target '${_t}' for INTERFACE_INCLUDE_DIRECTORIES")
      get_target_property(_rawincs "${_t}" INTERFACE_INCLUDE_DIRECTORIES)
      if(_rawincs)
        message(STATUS "StageHelpers:  raw INTERFACE_INCLUDE_DIRECTORIES('${_t}') = '${_rawincs}'")
        foreach(_entry IN LISTS _rawincs)
          if(_entry STREQUAL "") 
            continue() 
          endif()
          stage_resolve_include_entry(_entry_resolved "${_entry}")
          if(_entry_resolved AND EXISTS "${_entry_resolved}")
            list(APPEND _resolved_includes "${_entry_resolved}")
            message(STATUS "StageHelpers:   resolved include -> ${_entry_resolved}")
          else()
            message(STATUS "StageHelpers:   unresolved include entry for '${_t}': raw='${_entry}' -> resolved='${_entry_resolved}'")
          endif()
        endforeach()
      else()
        message(STATUS "StageHelpers:  target '${_t}' has no INTERFACE_INCLUDE_DIRECTORIES (property unset/empty)")
      endif()
    else()
      message(STATUS "StageHelpers:  target '${_t}' not found in project; skipping.")
    endif()
  endforeach()

  # Add vendor include dir if provided
  if(VENDOR_INCLUDE_DIR AND EXISTS "${VENDOR_INCLUDE_DIR}")
    file(REAL_PATH "${VENDOR_INCLUDE_DIR}" _vendor_real)
    list(APPEND _resolved_includes "${_vendor_real}")
    message(STATUS "StageHelpers: added vendor include dir: ${_vendor_real}")
  endif()

  # Conservative fallback: common third_party include locations
  if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/include")
    file(REAL_PATH "${CMAKE_SOURCE_DIR}/third_party/include" _tpinc)
    list(APPEND _resolved_includes "${_tpinc}")
    message(STATUS "StageHelpers: added fallback third_party/include -> ${_tpinc}")
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/fmt/include")
    file(REAL_PATH "${CMAKE_SOURCE_DIR}/third_party/fmt/include" _finc)
    list(APPEND _resolved_includes "${_finc}")
    message(STATUS "StageHelpers: added fallback fmt include -> ${_finc}")
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/libzmq/include")
    file(REAL_PATH "${CMAKE_SOURCE_DIR}/third_party/libzmq/include" _zinc)
    list(APPEND _resolved_includes "${_zinc}")
    message(STATUS "StageHelpers: added fallback libzmq include -> ${_zinc}")
  endif()

  list(REMOVE_DUPLICATES _resolved_includes)
  message(STATUS "StageHelpers: final resolved include roots: ${_resolved_includes}")

  # Configure-time collision check: ensure no duplicate relative paths across roots
  set(_seen_rel "")
  foreach(_dir IN LISTS _resolved_includes)
    if(NOT EXISTS "${_dir}") 
      continue() 
    endif()
    file(GLOB_RECURSE _fils RELATIVE "${_dir}" "${_dir}/*")
    foreach(_rf IN LISTS _fils)
      string(REGEX REPLACE "\\\\+" "/" _rf_norm "${_rf}")
      if(_rf_norm STREQUAL "") 
        continue() 
      endif()
      list(FIND _seen_rel "${_rf_norm}" _found)
      if(NOT _found EQUAL -1)
        message(FATAL_ERROR "StageHelpers: Include collision detected for '${_rf_norm}' present in multiple include roots. Resolve overlapping providers.")
      endif()
      list(APPEND _seen_rel "${_rf_norm}")
    endforeach()
  endforeach()

  # Schedule copy_directory to the given stage target (post-build)
  foreach(_dir IN LISTS _resolved_includes)
    if(EXISTS "${_dir}")
      add_custom_command(TARGET ${_target_stage} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_dir}" "${STAGE_INCLUDE}"
        COMMENT "StageHelpers: staging includes -> ${STAGE_INCLUDE} (from ${_dir})"
        VERBATIM
      )
      message(STATUS "StageHelpers: scheduled staging of include dir ${_dir} -> ${STAGE_INCLUDE}")
    endif()
  endforeach()

  # Expose resolved list into cache for debugging convenience
  set(_STAGEHELPERS_RESOLVED_INCLUDES "${_resolved_includes}" CACHE INTERNAL "StageHelpers: resolved include dirs")
endfunction()

# ---------------------------------------------------------------------------
# stage_schedule_third_party_libs(TARGETS_LIST STAGE_LIB TARGET_STAGE=...)
# Schedules copies of $<TARGET_FILE:...> for targets that exist; additionally
# probes a small list of common candidate target names for fallback.
# ---------------------------------------------------------------------------
function(stage_schedule_third_party_libs TARGETS_LIST STAGE_LIB)
  message(STATUS "******** stage_schedule_third_party_libs arguments:")
  cmake_parse_arguments(SH "" "" "TARGET_STAGE" ${ARGN})
  message(STATUS "         Arugments received: TARGETS_LIST: ${${TARGETS_LIST}}")
  message(STATUS "         Arugments received: STAGE_LIB: ${STAGE_LIB}")
  message(STATUS "         Arugments received: TARGET_STAGE: ${SH_TARGET_STAGE}")
  set(_target_stage "${SH_TARGET_STAGE}")
  if(NOT _target_stage)
    message(FATAL_ERROR "stage_schedule_third_party_libs: must provide TARGET_STAGE=<cmake-target>")
  endif()
  message(STATUS "StageHelpers: using TARGET_STAGE='${_target_stage}' for libs")

  set(_targets_val "")
  if(DEFINED ${TARGETS_LIST} AND NOT "${TARGETS_LIST}" STREQUAL "")
    set(_targets_val "${${TARGETS_LIST}}")
  else()
    set(_targets_val "${TARGETS_LIST}")
  endif()

  foreach(_t IN LISTS _targets_val)
    if(NOT _t) 
      continue() 
    endif()
    if(TARGET "${_t}")
      add_custom_command(TARGET ${_target_stage} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${_t}> "${STAGE_LIB}/"
        COMMENT "StageHelpers: staging binary for ${_t} -> ${STAGE_LIB}"
        VERBATIM
      )
      message(STATUS "StageHelpers: scheduled binary staging for target '${_t}' -> ${STAGE_LIB}")
    else()
      message(STATUS "StageHelpers: target '${_t}' not present; skipping binary staging")
    endif()
  endforeach()

  # conservative fallback probe
  set(_fallback_candidates
    "fmt" "fmt::fmt" "fmt::fmt-header-only" "libfmt" "libfmt.a"
    "libzmq-static" "libzmq" "zmq"
  )
  foreach(_cand IN LISTS _fallback_candidates)
    if(TARGET "${_cand}")
      add_custom_command(TARGET ${_target_stage} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${_cand}> "${STAGE_LIB}/"
        COMMENT "StageHelpers: staging fallback binary for ${_cand} -> ${STAGE_LIB}"
        VERBATIM
      )
      message(STATUS "StageHelpers: scheduled fallback binary staging for '${_cand}'")
    endif()
  endforeach()
endfunction()

# ---------------------------------------------------------------------------
# stage_schedule_lib_duplicate_check(STAGE_LIB TARGET_STAGE=...)
# Writes a small check script into the build tree and schedules it to run
# after staged lib copies. FATALs if duplicate basenames are found.
# ---------------------------------------------------------------------------
function(stage_schedule_lib_duplicate_check STAGE_LIB)
  message(STATUS "******** stage_schedule_lib_duplicate_check arguments:")
  cmake_parse_arguments(SH "" "" "TARGET_STAGE" ${ARGN})
  message(STATUS "         Arugments received: STAGE_LIB: ${${STAGE_LIB}}")
  message(STATUS "         Arugments received: TARGET_STAGE: ${SH_TARGET_STAGE}")
  set(_target_stage "${SH_TARGET_STAGE}")
  if(NOT _target_stage)
    message(FATAL_ERROR "stage_schedule_lib_duplicate_check: must provide TARGET_STAGE=<cmake-target>")
  endif()

  set(_dup_script "${CMAKE_BINARY_DIR}/cmake/check_stage_lib_duplicates.cmake")
  file(WRITE "${_dup_script}"
"# Auto-generated by cmake/StageHelpers.cmake\n"
"if(NOT EXISTS \"${STAGE_LIB}\")\n"
"  message(FATAL_ERROR \"StageHelpers: staged lib directory '${STAGE_LIB}' not found; ensure stage_install ran successfully.\")\n"
"endif()\n"
"file(GLOB _lib_files \"${STAGE_LIB}/*\")\n"
"set(_basenames \"\")\n"
"foreach(_f IN LISTS _lib_files)\n"
"  get_filename_component(_bn \"${_f}\" NAME)\n"
"  list(FIND _basenames \"${_bn}\" _idx)\n"
"  if(NOT _idx EQUAL -1)\n"
"    message(FATAL_ERROR \"StageHelpers: Duplicate library basename detected in staged lib directory: '${_bn}'\")\n"
"  else()\n"
"    list(APPEND _basenames \"${_bn}\")\n"
"  endif()\n"
"endforeach()\n"
"message(STATUS \"StageHelpers: Stage lib duplicate check: OK (no duplicate basenames found)\")\n"
)

  add_custom_command(TARGET ${_target_stage} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -P "${_dup_script}"
    COMMENT "StageHelpers: Checking staged lib directory for duplicate basenames..."
    VERBATIM
  )
  message(STATUS "StageHelpers: scheduled staged lib duplicate-check script -> ${_dup_script}")
endfunction()
