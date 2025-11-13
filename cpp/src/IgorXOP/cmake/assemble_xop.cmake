# cmake/assemble_xop.cmake
# ------------------------------------------------------------------------------
# Assemble a .xop bundle directory from a compiled module (Mach-O) file.
#
# Usage (example):
#   cmake -D_mach_o="/abs/path/pylabhubxop64" \
#         -D_bundle_dir="/abs/path/pylabhubxop64.xop" \
#         -D_xop_name="pylabhubxop64" \
#         -D_build_root="/abs/build/dir" \
#         -D_config="Release" \
#         -D_configured_plist="/abs/path/to/generated/Info.plist" \
#         -P assemble_xop.cmake
#
# Notes:
#  - _mach_o, _bundle_dir and _xop_name are required.
#  - _build_root and _config are optional but recommended for Info.plist discovery.
#  - configured_plist (explicit path) is optional and has highest priority for Info.plist.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)

# -----------------------
# Logging helper
# -----------------------
function(_log level msg)
  if(level STREQUAL "STATUS")
    message(STATUS "${msg}")
  elseif(level STREQUAL "WARNING")
    message(WARNING "${msg}")
  else()
    message("${msg}")
  endif()
endfunction()

# -----------------------
# Sanitization helper
# Remove surrounding quotes (single/double) and trim whitespace
# -----------------------
function(_strip_surrounding_quotes out_var in_var)
  if(NOT DEFINED ${in_var})
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  set(_tmp "${${in_var}}")
  string(STRIP "${_tmp}" _tmp)
  if(_tmp MATCHES "^\".*\"$" OR _tmp MATCHES "^'.*'$")
    string(REGEX REPLACE "^['\"](.*)['\"]$" "\\1" _tmp "${_tmp}")
  else()
    string(REGEX REPLACE "^\"+(.*)\"+$" "\\1" _tmp "${_tmp}")
    string(REGEX REPLACE "^'+(.*)'+$" "\\1" _tmp "${_tmp}")
  endif()
  set(${out_var} "${_tmp}" PARENT_SCOPE)
endfunction()

# -----------------------
# Read incoming -D variables (they may be quoted by generator like Xcode)
# -----------------------
if(DEFINED _mach_o)            
  set(_raw_mach_o "${_mach_o}")         
else() 
  set(_raw_mach_o "") 
endif()
if(DEFINED _bundle_dir)        
  set(_raw_bundle_dir "${_bundle_dir}") 
else() 
  set(_raw_bundle_dir "") 
endif()
if(DEFINED _xop_name)          
  set(_raw_xop_name "${_xop_name}")     
else() 
  set(_raw_xop_name "") 
endif()
if(DEFINED _build_root)        
  set(_raw_build_root "${_build_root}") 
else() 
  set(_raw_build_root "") 
endif()
if(DEFINED _config)            
  set(_raw_config "${_config}")         
else() 
  set(_raw_config "") 
endif()
if(DEFINED configured_plist)   
  set(_raw_configured_plist "${configured_plist}") 
else() 
  set(_raw_configured_plist "") 
endif()

_log(STATUS "assemble_xop.cmake: starting (will sanitize inputs)")

# sanitize
_strip_surrounding_quotes(_mach_o_sanitized _raw_mach_o)
_strip_surrounding_quotes(_bundle_dir_sanitized _raw_bundle_dir)
_strip_surrounding_quotes(_xop_name_sanitized _raw_xop_name)
_strip_surrounding_quotes(_build_root_sanitized _raw_build_root)
_strip_surrounding_quotes(_config_sanitized _raw_config)
_strip_surrounding_quotes(_configured_plist_sanitized _raw_configured_plist)

_log(STATUS "assemble_xop.cmake: sanitized parameters:")
_log(STATUS "  _mach_o   = '${_mach_o_sanitized}'")
_log(STATUS "  _bundle_dir='${_bundle_dir_sanitized}'")
_log(STATUS "  _xop_name = '${_xop_name_sanitized}'")
_log(STATUS "  _build_root='${_build_root_sanitized}'")
_log(STATUS "  _config   = '${_config_sanitized}'")
if(NOT "${_configured_plist_sanitized}" STREQUAL "") 
  _log(STATUS "  configured_plist (explicit)='${_configured_plist_sanitized}'")
endif()

# canonical names for script
set(built_target_input "${_mach_o_sanitized}")
set(desired_bundle_dir_input "${_bundle_dir_sanitized}")
set(bundle_name "${_xop_name_sanitized}")
set(build_root "${_build_root_sanitized}")
set(build_config "${_config_sanitized}")
set(configured_plist_arg "${_configured_plist_sanitized}")

# -----------------------
# Validate required inputs
# -----------------------
if(built_target_input STREQUAL "" OR desired_bundle_dir_input STREQUAL "" OR bundle_name STREQUAL "")
  message(FATAL_ERROR "assemble_xop.cmake: missing required -D arguments.
Required:
  -D_mach_o
  -D_bundle_dir
  -D_xop_name
Optional:
  -D_build_root -D_config -D_configured_plist")
endif()

# -----------------------
# Resolve actual built artifact (robust to suffix differences)
# -----------------------
get_filename_component(built_target_abs "${built_target_input}" ABSOLUTE)
get_filename_component(built_dir "${built_target_abs}" DIRECTORY)
get_filename_component(built_basename "${built_target_abs}" NAME_WE)

_log(STATUS "assemble_xop.cmake: resolving built artifact")
_log(STATUS "  requested = ${built_target_input}")
_log(STATUS "  normalized= ${built_target_abs}")
_log(STATUS "  dir       = ${built_dir}")
_log(STATUS "  base(w/o ext) = ${built_basename}")

# show directory contents for diagnostics (helpful in Xcode logs)
execute_process(COMMAND /bin/ls -la "${built_dir}"
                RESULT_VARIABLE _ls_res
                OUTPUT_VARIABLE _ls_out
                ERROR_VARIABLE _ls_err
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_ls_res EQUAL 0)
  _log(STATUS "  directory listing:\n${_ls_out}")
else()
  _log(WARNING "  could not list '${built_dir}': ${_ls_err}")
endif()

# exact-match ok
if(EXISTS "${built_target_abs}")
  set(resolved_built_target "${built_target_abs}")
else()
  # try typical alternative suffixes that might be produced by various linkers/generators
  set(_alt_suffixes ".xop" ".xop.bundle" ".so" ".dylib" "")
  set(resolved_built_target "")
  foreach(_s IN LISTS _alt_suffixes)
    if("${_s}" STREQUAL "")
      set(_candidate "${built_dir}/${built_basename}")
    else()
      set(_candidate "${built_dir}/${built_basename}${_s}")
    endif()
    if(EXISTS "${_candidate}")
      set(resolved_built_target "${_candidate}")
      _log(STATUS "assemble_xop.cmake: found candidate built artifact: ${resolved_built_target}")
      break()
    endif()
  endforeach()
endif()

if(NOT resolved_built_target)
  message(FATAL_ERROR "assemble_xop.cmake: built target not found.
                        Primary: ${built_target_input}
                        Normalized: ${built_target_abs}
                        Directory checked: ${built_dir}
                        Observed listing:\n${_ls_out}
                        Please ensure the link step produced the module and that this script runs after link.")
endif()

get_filename_component(resolved_built_target_abs "${resolved_built_target}" ABSOLUTE)
_log(STATUS "assemble_xop.cmake: using resolved artifact: ${resolved_built_target_abs}")

# -----------------------
# Bundle paths (ensure .xop suffix)
# -----------------------
get_filename_component(desired_bundle_dir_abs "${desired_bundle_dir_input}" ABSOLUTE)
string(REGEX MATCH "\\.xop$" _has_xop_suffix "${desired_bundle_dir_abs}")
if(NOT _has_xop_suffix)
  set(desired_bundle_dir_abs "${desired_bundle_dir_abs}.xop")
endif()

set(bundle_contents "${desired_bundle_dir_abs}/Contents")
set(bundle_macos_dir "${bundle_contents}/MacOS")
set(bundle_resources_dir "${bundle_contents}/Resources")

_log(STATUS "assemble_xop.cmake: bundle dest = ${desired_bundle_dir_abs}")

# -----------------------
# Info.plist discovery helper (ordered)
# Priority:
#  1) -D_configured_plist (explicit)
#  2) <build_root>/src/IgorXOP/Info.plist
#  3) <build_root>/src/IgorXOP/<config>/Info.plist
#  4) <CMAKE_BINARY_DIR>/src/IgorXOP/Info.plist
#  5) source template Info64.plist.in (treated as template)
# -----------------------
function(_choose_configured_plist out_plist out_is_template)
  set(_found "")
  set(_is_template OFF)
  # 1) explicit
  if(NOT "${configured_plist_arg}" STREQUAL "")
    if(EXISTS "${configured_plist_arg}")
      set(_found "${configured_plist_arg}")
      set(_is_template OFF)
    endif()
  endif()

  # 2) build-root locations
  if(_found STREQUAL "" AND NOT "${build_root}" STREQUAL "")
    if(EXISTS "${build_root}/src/IgorXOP/Info.plist")
      set(_found "${build_root}/src/IgorXOP/Info.plist")
      set(_is_template OFF)
    elseif(NOT "${build_config}" STREQUAL "" AND EXISTS "${build_root}/src/IgorXOP/${build_config}/Info.plist")
      set(_found "${build_root}/src/IgorXOP/${build_config}/Info.plist")
      set(_is_template OFF)
    endif()
  endif()

  # 3) CMAKE_BINARY_DIR fallback
  if(_found STREQUAL "")
    if(EXISTS "${CMAKE_BINARY_DIR}/src/IgorXOP/Info.plist")
      set(_found "${CMAKE_BINARY_DIR}/src/IgorXOP/Info.plist")
      set(_is_template OFF)
    elseif(NOT "${build_config}" STREQUAL "" AND EXISTS "${CMAKE_BINARY_DIR}/src/IgorXOP/${build_config}/Info.plist")
      set(_found "${CMAKE_BINARY_DIR}/src/IgorXOP/${build_config}/Info.plist")
      set(_is_template OFF)
    endif()
  endif()

  # 4) source template
  if(_found STREQUAL "")
    set(_tpl "${CMAKE_SOURCE_DIR}/src/IgorXOP/Info64.plist.in")
    if(EXISTS "${_tpl}")
      set(_found "${_tpl}")
      set(_is_template ON)
    endif()
  endif()

  # return
  if(NOT _found STREQUAL "")
    set(${out_plist} "${_found}" PARENT_SCOPE)
    set(${out_is_template} "${_is_template}" PARENT_SCOPE)
  else()
    set(${out_plist} "" PARENT_SCOPE)
    set(${out_is_template} "OFF" PARENT_SCOPE)
  endif()
endfunction()

_choose_configured_plist(chosen_plist chosen_is_template)
if(chosen_plist)
  if(chosen_is_template)
    _log(STATUS "assemble_xop.cmake: chosen Info.plist: template ${chosen_plist}")
  else()
    _log(STATUS "assemble_xop.cmake: chosen configured Info.plist: ${chosen_plist}")
  endif()
else()
  _log(STATUS "assemble_xop.cmake: no configured Info.plist found; will write minimal plist")
endif()

# -----------------------
# If resolved artifact is already a bundle dir, do idempotent check / ensure Info.plist and return
# -----------------------
if(IS_DIRECTORY "${resolved_built_target_abs}")
  if(EXISTS "${resolved_built_target_abs}/Contents/MacOS")
    _log(STATUS "assemble_xop.cmake: artifact is already a bundle: ${resolved_built_target_abs}")
    # try to ensure Info.plist exists
    if(NOT EXISTS "${resolved_built_target_abs}/Contents/Info.plist")
      if(chosen_plist AND NOT chosen_is_template)
        file(COPY "${chosen_plist}" DESTINATION "${resolved_built_target_abs}/Contents")
        _log(STATUS "assemble_xop.cmake: copied configured Info.plist -> ${resolved_built_target_abs}/Contents/Info.plist")
      elseif(chosen_plist AND chosen_is_template)
        configure_file("${chosen_plist}" "${resolved_built_target_abs}/Contents/Info.plist" @ONLY)
        _log(STATUS "assemble_xop.cmake: configured template -> ${resolved_built_target_abs}/Contents/Info.plist")
      else()
        _log(WARNING "assemble_xop.cmake: existing bundle missing Info.plist and no configured plist/template found")
      endif()
    else()
      _log(STATUS "assemble_xop.cmake: bundle already contains Info.plist")
    endif()
    _log(STATUS "assemble_xop.cmake: ready: ${resolved_built_target_abs}")
    return()
  endif()
endif()

# -----------------------
# Create bundle directories and copy executable
# -----------------------
_log(STATUS "assemble_xop.cmake: creating bundle directories")
file(MAKE_DIRECTORY "${bundle_macos_dir}")
file(MAKE_DIRECTORY "${bundle_resources_dir}")

# pick executable name (prefer env var XOP_BUNDLE_EXECUTABLE)
if(DEFINED ENV{XOP_BUNDLE_EXECUTABLE} AND NOT "$ENV{XOP_BUNDLE_EXECUTABLE}" STREQUAL "")
  set(dest_exec_name "$ENV{XOP_BUNDLE_EXECUTABLE}")
else()
  get_filename_component(dest_exec_name "${resolved_built_target_abs}" NAME)
endif()
set(dest_exec_path "${bundle_macos_dir}/${dest_exec_name}")

file(COPY "${resolved_built_target_abs}" DESTINATION "${bundle_macos_dir}")
_log(STATUS "assemble_xop.cmake: copied '${resolved_built_target_abs}' -> '${dest_exec_path}'")

# ensure exec bit
execute_process(COMMAND /bin/chmod +x "${dest_exec_path}"
                RESULT_VARIABLE _chmod_res
                OUTPUT_VARIABLE _chmod_out
                ERROR_VARIABLE _chmod_err)
if(NOT _chmod_res EQUAL 0)
  _log(WARNING "could not set exec bit on ${dest_exec_path}: ${_chmod_err}")
endif()

# -----------------------
# Install/configure Info.plist into bundle
# -----------------------
if(chosen_plist AND NOT chosen_is_template)
  file(COPY "${chosen_plist}" DESTINATION "${bundle_contents}")
  _log(STATUS "assemble_xop.cmake: copied configured Info.plist -> ${bundle_contents}/Info.plist")
elseif(chosen_plist AND chosen_is_template)
  configure_file("${chosen_plist}" "${bundle_contents}/Info.plist" @ONLY)
  _log(STATUS "assemble_xop.cmake: configured template -> ${bundle_contents}/Info.plist")
else()
  _log(WARNING "assemble_xop.cmake: no configured Info.plist found; writing minimal Info.plist")
  file(WRITE "${bundle_contents}/Info.plist"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>\n"
    "  <key>CFBundlePackageType</key><string>IXOP</string>\n"
    "  <key>CFBundleName</key><string>${bundle_name}</string>\n"
    "  <key>CFBundleExecutable</key><string>${dest_exec_name}</string>\n"
    "</dict></plist>\n")
endif()

if(EXISTS "${bundle_contents}/Info.plist")
  _log(STATUS "assemble_xop.cmake: Info.plist ready at ${bundle_contents}/Info.plist")
else()
  _log(WARNING "assemble_xop.cmake: Info.plist missing after install/write")
endif()

# -----------------------
# Optional: copy .r resources from source tree if present
# -----------------------
set(src_resources_dir "${CMAKE_SOURCE_DIR}/src/IgorXOP")
file(GLOB r_files RELATIVE "${src_resources_dir}" "${src_resources_dir}/*.r")
if(r_files)
  foreach(_r IN LISTS r_files)
    file(COPY "${src_resources_dir}/${_r}" DESTINATION "${bundle_resources_dir}")
  endforeach()
  _log(STATUS "assemble_xop.cmake: copied resources -> ${bundle_resources_dir}/")
endif()

# -----------------------
# Success
# -----------------------
_log(STATUS "assemble_xop.cmake: created .xop bundle at ${desired_bundle_dir_abs}")
