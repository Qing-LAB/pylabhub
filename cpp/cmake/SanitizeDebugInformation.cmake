# === Sanitized debug/file prefix mapping (clean, canonical, deterministic) ===
# Place this after project(...) and after your platform detection block.

# Canonicalize a path if provided. If it exists return realpath; else return absolute path.
function(_canon_if_exists out_var in_path)
  if(NOT in_path OR "${in_path}" STREQUAL "")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  if(EXISTS "${in_path}")
    file(REAL_PATH "${in_path}" _real_path)
    set(${out_var} "${_real_path}" PARENT_SCOPE)
  else()
    get_filename_component(_abs "${in_path}" ABSOLUTE)
    set(${out_var} "${_abs}" PARENT_SCOPE)
  endif()
endfunction()

# Literal "is prefix" check (no regex) — TRUE if candidate == prefix or candidate starts with prefix + "/"
function(_is_under prefix candidate out_var)
  if(NOT prefix OR NOT candidate)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  string(LENGTH "${prefix}" _plen)
  string(LENGTH "${candidate}" _clen)
  if(_plen EQUAL 0 OR _clen LESS _plen)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  string(SUBSTRING "${candidate}" 0 ${_plen} _sub)
  if(NOT _sub STREQUAL "${prefix}")
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  if(_clen EQUAL _plen)
    set(${out_var} TRUE PARENT_SCOPE)
    return()
  endif()
  string(SUBSTRING "${candidate}" ${_plen} 1 _next_char)
  if("${_next_char}" STREQUAL "/")
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(print_list msg list_var)
  message(STATUS " -- ${msg}:")
  foreach(item IN LISTS ${list_var})
      message(STATUS "    -- ${item}")
  endforeach()
endfunction()

function(print_list_pairs msg list1_var list2_var)
  message(STATUS " -- ${msg}:")
  if(NOT DEFINED ${list1_var})
      message(FATAL_ERROR " -- List '${list1_var}' is not defined")
  endif()

  if(NOT DEFINED ${list2_var})
      message(FATAL_ERROR " -- List '${list2_var}' is not defined")
  endif()

  list(LENGTH ${list1_var} len1)
  list(LENGTH ${list2_var} len2)

  if(NOT len1 EQUAL len2)
      message(FATAL_ERROR
          " -- List length mismatch: '${list1_var}'=${len1}, '${list2_var}'=${len2}"
      )
  endif()

  if(len1 LESS_EQUAL 0)
    message(STATUS "      -- empty list")
    return()
  endif()

  math(EXPR last "${len1} - 1")
  foreach(i RANGE 0 ${last})
      list(GET ${list1_var} ${i} item1)
      list(GET ${list2_var} ${i} item2)
      message(STATUS "    -- ${item1} [as ${item2}]")
  endforeach()
endfunction()

# _collect_implicit_paths_with_origin(var_name origin out_paths_var out_origins_var)
#
# Helper function to collect implicit compiler/linker paths.
# This function canonicalizes paths from a given list variable and appends them
# to a pair of output lists (`out_paths_var`, `out_origins_var`) in the parent scope.
#
# Arguments:
#   var_name: The name of the CMake list variable (e.g., CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
#             containing paths to process.
#   origin:   The string label for the origin of these paths (e.g., "IMP_INC", "IMP_LIB").
#   out_paths_var:   The name of the list variable in the parent scope to append canonicalized paths to.
#   out_origins_var: The name of the list variable in the parent scope to append path origins to.
#
function(_collect_implicit_paths_with_origin var_name origin out_paths_var out_origins_var)
  set(local_paths "${${out_paths_var}}") # Dereference the list from the parent
  set(local_origins "${${out_origins_var}}")
  if(DEFINED ${var_name})
    foreach(_path IN LISTS ${${var_name}})
      if(_path AND NOT "${_path}" STREQUAL "")
        _canon_if_exists(_p "${_path}")
        if(_p)
          list(APPEND local_paths "${_p}")
          list(APPEND local_origins "${origin}")
        endif()
      endif()
    endforeach()
  endif()
  set(${out_paths_var} "${local_paths}" PARENT_SCOPE) # Set the result back to the parent
  set(${out_origins_var} "${local_origins}" PARENT_SCOPE)
endfunction()


message(STATUS "=============================================================================")
message(STATUS " Setting up sanitization of file and path names for debug information")

# Canonicalize source and binary roots once and reuse canonical values everywhere.
_canon_if_exists(_canon_source "${CMAKE_SOURCE_DIR}")
_canon_if_exists(_canon_binary "${CMAKE_BINARY_DIR}")

# Collect canonical prefixes into parallel lists _paths[] and _origins[]
set(_paths "")
set(_origins "")

message(STATUS "[Sanitize debug information] canonical CMAKE_SOURCE_DIR = ${_canon_source}")
message(STATUS "[Sanitize debug information] canonical CMAKE_BINARY_DIR = ${_canon_binary}")

# Project build roots (use canonical values)
if(_canon_binary)
  list(APPEND _paths "${_canon_binary}")
  list(APPEND _origins "BUILD")
endif()

# Project source and binary roots (use canonical values)
if(_canon_source)
  list(APPEND _paths "${_canon_source}")
  list(APPEND _origins "SOURCE")
endif()

# TOOLCHAIN (heuristic: two parents above compiler binary)
if(CMAKE_CXX_COMPILER)
  get_filename_component(_cxx_bin "${CMAKE_CXX_COMPILER}" ABSOLUTE)
  get_filename_component(_cxx_parent "${_cxx_bin}" DIRECTORY)
  get_filename_component(_cxx_root "${_cxx_parent}" DIRECTORY)
  if(_cxx_root AND NOT "${_cxx_root}" STREQUAL "")
    _canon_if_exists(_p "${_cxx_root}")
    if(_p)
      list(APPEND _paths "${_p}")
      list(APPEND _origins "TOOLCHAIN")
      message(STATUS "[Sanitize debug information] canonical TOOLCHAIN = ${_p}")
    endif()
  endif()
endif()

# SYSROOT
if(DEFINED CMAKE_SYSROOT AND NOT "${CMAKE_SYSROOT}" STREQUAL "")
  _canon_if_exists(_p "${CMAKE_SYSROOT}")
  if(_p)
    list(APPEND _paths "${_p}")
    list(APPEND _origins "SYSROOT")
    message(STATUS "[Sanitize debug information] canonical SYSROOT = ${_p}")
  endif()
endif()

# CONDA
if(DEFINED ENV{CONDA_PREFIX} AND NOT "$ENV{CONDA_PREFIX}" STREQUAL "")
  _canon_if_exists(_p "$ENV{CONDA_PREFIX}")
  if(_p)
    list(APPEND _paths "${_p}")
    list(APPEND _origins "CONDA")
    message(STATUS "[Sanitize debug information] canonical CONDA = ${_p}")
  endif()
endif()

# VCTOOLS
if(DEFINED ENV{VCToolsInstallDir} AND NOT "$ENV{VCToolsInstallDir}" STREQUAL "")
  _canon_if_exists(_p "$ENV{VCToolsInstallDir}")
  if(_p)
    list(APPEND _paths "${_p}")
    list(APPEND _origins "VCTOOLS")
    message(STATUS "[Sanitize debug information] canonical VCTOOLS = ${_p}")
  endif()
endif()

# Implicit include and link directories (CMake >= 3.20)
_collect_implicit_paths_with_origin(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "IMP_INC" _paths _origins)
_collect_implicit_paths_with_origin(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES "IMP_LIB" _paths _origins)

# Candidate HOME (consider later — only add if nothing else under HOME)
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  _canon_if_exists(_home "$ENV{HOME}")
else()
  set(_home "")
endif()

# Deduplicate by path (keep first origin seen)
set(_uniq_paths "")
set(_uniq_origins "")
list(LENGTH _paths _len)
set(_i 0)
while(_i LESS _len)
  list(GET _paths ${_i} _path)
  list(GET _origins ${_i} _orig)
  if(NOT _path IN_LIST _uniq_paths)
    list(APPEND _uniq_paths "${_path}")
    list(APPEND _uniq_origins "${_orig}")
  endif()
  math(EXPR _i "${_i} + 1")
endwhile()
set(_paths ${_uniq_paths})
set(_origins ${_uniq_origins})

# Add HOME only if no other collected path is under HOME (avoid common-parent trap)
if(_home)
  set(_add_home TRUE)
  list(LENGTH _paths _plen)
  set(_j 0)
  while(_j LESS _plen)
    list(GET _paths ${_j} _ptest)
    _is_under("${_home}" "${_ptest}" _u)
    if(_u)
      set(_add_home FALSE)
      break()
    endif()
    math(EXPR _j "${_j} + 1")
  endwhile()
  if(_add_home)
    list(APPEND _paths "${_home}")
    list(APPEND _origins "HOME")
  endif()
endif()

# Optional: filter out system roots like "/" and "/usr"
set(_kept_paths "")
set(_kept_origins "")
list(LENGTH _paths _plen)
set(_i 0)
while(_i LESS _plen)
  list(GET _paths ${_i} _p)
  list(GET _origins ${_i} _o)
  if(NOT _p STREQUAL "/")
    if(NOT _p MATCHES "^/usr(/|$)")
      list(APPEND _kept_paths "${_p}")
      list(APPEND _kept_origins "${_o}")
    endif()
  endif()
  math(EXPR _i "${_i} + 1")
endwhile()
set(_paths ${_kept_paths})
set(_origins ${_kept_origins})

# Build an index list (0..N-1)
set(_indices "")
list(LENGTH _paths _plen)
if(_plen GREATER 0)
  math(EXPR _imax "${_plen} - 1")
  foreach(_ii RANGE 0 ${_imax})
    list(APPEND _indices "${_ii}")
  endforeach()
endif()

print_list_pairs("[Sanitize debug information] The following directories will be examined:" _paths _origins)
message(STATUS "")

# -----------------------------------------------------------------------------
# process_candidate: remove kept entries under candidate_path matching child-origins
# - final_paths_var / final_origins_var : names of in-place lists (updated via PARENT_SCOPE)
# - candidate_path, candidate_origin : strings
# - ARGN : list of child origins to remove (support wildcard "*" to match any origin)
# -----------------------------------------------------------------------------
function(process_candidate final_paths_var final_origins_var candidate_path candidate_origin)  
  set(_child_origins ${ARGN})
  set(_fpaths "${${final_paths_var}}")
  set(_forigs "${${final_origins_var}}")
  # message(STATUS " --** examining ${candidate_path} (origin=${candidate_origin})")
  
  # detect wildcard presence
  set(_has_wildcard FALSE)
  foreach(_corg IN LISTS _child_origins)
    if("${_corg}" STREQUAL "*")
      set(_has_wildcard TRUE)
      break()
    endif()
  endforeach()

  list(LENGTH _fpaths _flen)
  set(_k 0)
  # message(STATUS "${_flen} ${_k}")
  while(_k LESS _flen)
    list(GET _fpaths ${_k} _kp)
    list(GET _forigs ${_k} _ko)
    # message(STATUS "      ** evaluating ${_kp} (origin=${_ko}), wild_card is ${_has_wildcard}")
    # decide if this existing entry should be considered for removal
    set(_origin_match FALSE)
    if(_has_wildcard)
      set(_origin_match TRUE)
    else()
      foreach(_corg IN LISTS _child_origins)
        if("${_ko}" STREQUAL "${_corg}")
          set(_origin_match TRUE)
          break()
        endif()
      endforeach()
    endif()

    if(_origin_match)
      
      # test path containment using your existing helper
      _is_under("${candidate_path}" "${_kp}" _under)
      if(_under)
        # remove this kept entry (do not increment index)
        list(REMOVE_AT _fpaths ${_k})
        list(REMOVE_AT _forigs ${_k})
        list(LENGTH _fpaths _flen)
        # message(STATUS "                          ... removed...")
        continue()
      endif()
    endif()

    math(EXPR _k "${_k} + 1")
    # message(STATUS "                          ... moving on ...")
  endwhile()

  set(${final_paths_var} "${_fpaths}" PARENT_SCOPE)
  set(${final_origins_var} "${_forigs}" PARENT_SCOPE)
endfunction()


# Build final lists, preferring SOURCE over BUILD when parent/child conflicts exist.
set(_final_paths "")
set(_final_origins "")
set(_shrink_paths ${_paths})
set(_shrink_origins ${_origins})

# print_list_pairs("[These are the _paths and _origins]" _shrink_paths _shrink_origins)
# message(STATUS "")
# -----------------------------------------------------------------------------
# Replacement loop: use process_candidate with wildcard where desired.
# Policy used here (adjust if you want different removals):
#  - SOURCE  : remove any kept child under it -> use "*"
#  - TOOLCHAIN: remove any children under it
#  - CONDA   : remove any children under it
#  - Others  : keep prior behavior (skip candidate if it's a parent of any kept path)
# -----------------------------------------------------------------------------
list(LENGTH _shrink_paths _remaining_len)
while(_remaining_len GREATER 0)
  list(LENGTH _shrink_paths _remaining_len)
  if(_remaining_len LESS_EQUAL 0)
    break()
  endif()
  list(GET _shrink_paths 0 _pp)
  list(GET _shrink_origins 0 _oo)
  list(REMOVE_AT _shrink_paths 0)
  list(REMOVE_AT _shrink_origins 0)
  list(LENGTH _shrink_paths _remaining_len)

  # print_list_pairs("[before processing we have these directories to futher examine]" _shrink_paths _shrink_origins)
  # message(STATUS "  remaining length: ${_remaining_len}")
  # print_list_pairs("[before processing, current _final_paths is]" _final_paths _final_origins)
  # message(STATUS "  continue to process: ${_pp} (origin=${_oo})")
  # message(STATUS "")

  if("${_oo}" STREQUAL "SOURCE")
    # remove any kept entries under this SOURCE, then append SOURCE
    process_candidate(_shrink_paths _shrink_origins "${_pp}" "${_oo}" "BUILD")
    list(APPEND _final_paths "${_pp}")
    list(APPEND _final_origins "${_oo}")
    continue()
  elseif("${_oo}" STREQUAL "TOOLCHAIN")
    # remove any entries under this TOOLCHAIN, then append TOOLCHAIN
    process_candidate(_shrink_paths _shrink_origins "${_pp}" "${_oo}" "*")
    list(APPEND _final_paths "${_pp}")
    list(APPEND _final_origins "${_oo}")
    continue()
  elseif("${_oo}" STREQUAL "CONDA")
    # remove any entries under this CONDA prefix, then append CONDA
    process_candidate(_shrink_paths _shrink_origins "${_pp}" "${_oo}" "*")
    list(APPEND _final_paths "${_pp}")
    list(APPEND _final_origins "${_oo}")
    continue()
  elseif("${_oo}" STREQUAL "VCTOOLS")
    # remove any entries under this CONDA prefix, then append CONDA
    process_candidate(_shrink_paths _shrink_origins "${_pp}" "${_oo}" "*")
    list(APPEND _final_paths "${_pp}")
    list(APPEND _final_origins "${_oo}")
    continue()
  endif()

  # Non-special candidate: skip it if it's a parent of any already-kept path.
  set(_is_parent FALSE)
  foreach(_kp IN LISTS _shrink_paths)
    _is_under("${_pp}" "${_kp}" _u2)
    if(_u2)
      set(_is_parent TRUE)
      # message(STATUS "   [detected] ${_pp} is the parent of ${_kp}, skipping")
      break()
    endif()
  endforeach()

  if(NOT _is_parent)
    list(APPEND _final_paths "${_pp}")
    list(APPEND _final_origins "${_oo}")
  endif()
endwhile()


# -----------------------------
# Sanity check: lengths must match
list(LENGTH _final_paths _flen)
list(LENGTH _final_origins _olen)
if(NOT _flen EQUAL _olen)
  message(FATAL_ERROR "[Sanitize debug information] internal error: _final_paths/_final_origins length mismatch: ${_flen} != ${_olen}")
endif()

# Sort final pairs by descending path length (selection)
if(_flen GREATER 0)
  if(_flen GREATER 1)
    math(EXPR _flen_1 "${_flen} - 1")
    foreach(_i RANGE 0 ${_flen_1})
      list(APPEND _idxs ${_i})
    endforeach()
  else()
    set(_idxs "0")
  endif()
else()
  set(_idxs "")
endif()
set(_emit_order "")
set(_shortest_len -1)
list(LENGTH _idxs _ilen)

while(_ilen GREATER 0)
  set(_best_pos -1)
  set(_best_len 9999)
  list(LENGTH _idxs _ilen)
  foreach(_pos RANGE 0 ${_ilen})
    if(${_pos} GREATER_EQUAL ${_ilen})
      break()
    endif()
    list(GET _idxs ${_pos} _ival)
    list(GET _final_paths ${_ival} _ipath)
    # message(STATUS " ** currently shortest length is ${_shortest_len}")
    string(LENGTH "${_ipath}" _l)
    # message(STATUS " ** examining ${_ipath}, len is: ${_l}")
    if(_l LESS_EQUAL _best_len AND _l GREATER_EQUAL _shortest_len)
      set(_best_len ${_l})
      set(_best_pos ${_pos})
    endif()
  endforeach()
  
  if(_best_pos EQUAL -1)
    break()
  endif()
  set(_shortest_len ${_best_len})
  
  list(GET _idxs ${_best_pos} _chosen)
  # message(STATUS " ** chosen one is ${_chosen}")

  list(APPEND _emit_order "${_chosen}")
  list(REMOVE_AT _idxs ${_best_pos})
  # message(STATUS "  ** current _emit_order is ${_emit_order}")
  # message(STATUS "  ** current _idxs is ${_idxs}")
  list(LENGTH _idxs _ilen)
endwhile()

list(REVERSE _emit_order)

set(_remap_flags "")
foreach(_i ${_emit_order})
  list(GET _final_paths ${_i} _p)
  list(GET _final_origins ${_i} _o)

  if("${_o}" STREQUAL "SOURCE")
    set(_to "/{SOURCE}")
  elseif("${_o}" STREQUAL "BUILD")
    set(_to "/{BUILD}")
  elseif("${_o}" STREQUAL "CONDA")
    set(_to "/{CONDA}")
  elseif("${_o}" STREQUAL "VCTOOLS")
    set(_to "/{VCTOOLS}")
  elseif("${_o}" STREQUAL "TOOLCHAIN")
    set(_to "/{TOOLCHAIN}")
  elseif("${_o}" STREQUAL "SYSROOT")
    set(_to "/{SYSROOT}")
  elseif("${_o}" STREQUAL "IMP_INC")
    set(_to "/{IMP_INC}")
  elseif("${_o}" STREQUAL "IMP_LIB")
    set(_to "/{IMP_LIB}")
  else()
    set(_to "/{HOME}")
  endif()

  list(APPEND _remap_flags "-ffile-prefix-map=${_p}=${_to}")
  list(APPEND _remap_flags "-fdebug-prefix-map=${_p}=${_to}")
endforeach()

# Diagnostics and apply flags
if(_remap_flags)
  
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(${_remap_flags})
    message(STATUS "[Sanitized debug information] Will pass file/debug prefix map flags:")
    foreach(_f IN LISTS _remap_flags)
      message(STATUS "  ${_f}")
    endforeach()

  elseif(MSVC)
    # MSVC uses /pathmap:old=new. We convert both -ffile-prefix-map and
    # -fdebug-prefix-map to /pathmap, as MSVC does not separate the concepts,
    # and then deduplicate the result for robustness.
    set(_msvc_remap_flags "")
    foreach(_flag IN LISTS _remap_flags)
      # Extract original path and Unix-style remapped path
      string(REGEX REPLACE "^-f(file|debug)-prefix-map=(.*)=(/.*)$" "\\2" _p_extracted "${_flag}")
      string(REGEX REPLACE "^-f(file|debug)-prefix-map=(.*)=(/.*)$" "\\3" _to_unix_extracted "${_flag}")

      # Now, convert _to_unix_extracted to MSVC-friendly format using a fake hostname
      # Assuming _to_unix_extracted is like "/{SOURCE}"
      string(REGEX REPLACE "/{(.*)}" "//ROOT/{\\1}" _to_msvc_converted "${_to_unix_extracted}")

      # Construct the MSVC flag
      list(APPEND _msvc_remap_flags "/pathmap:${_p_extracted}=${_to_msvc_converted}")
    endforeach()

    if(_msvc_remap_flags)
      list(REMOVE_DUPLICATES _msvc_remap_flags)
      # /pathmap requires /experimental:deterministic for MSVC to be effective.
      add_compile_options(${_msvc_remap_flags} /experimental:deterministic)
    endif()

    message(STATUS "[Sanitized debug information] Will pass file/debug prefix map flags:")
    foreach(_f IN LISTS _msvc_remap_flags)
      message(STATUS "  ${_f}")
    endforeach()
  endif()
  
else()
  message(STATUS "[Sanitized debug information] No remap flags generated.")
endif()

message(STATUS "===========================================================================")
message(STATUS "")
