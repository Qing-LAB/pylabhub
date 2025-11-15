# cmake/assemble_xop.cmake
# ------------------------------------------------------------------------------
# Assemble a macOS .xop bundle directory from a compiled module file.
#
# This script is designed to be invoked by CMake (via -P) with a small set of
# -D properties passed on the command line. It is intentionally robust to the
# variety of build outputs/linker suffixes that different generators produce
# (e.g. no-suffix module, .so, .dylib, .xop).
#
# Expected invocation (example):
#   cmake -D_mach_o="/abs/path/to/pylabhubxop64" \
#         -D_bundle_dir="/abs/path/to/pylabhubxop64.xop" \
#         -D_xop_name="pylabhubxop64" \
#         -D_build_root="/abs/build/dir" \
#         -D_config="Release" \
#         -D_configured_plist="/abs/path/to/generated/Info.plist" \
#         -D_r_file="/abs/path/to/WaveAccess.r" \
#         -D_r_include_dirs="/path/to/XOPSupport/include;/another/include" \
#         -D_rsrc_basename="pylabhubxop64.rsrc" \
#         -D_install_root="/abs/virtual/root" \
#         -P cmake/assemble_xop.cmake
#
# Required -D arguments:
#   -D_mach_o        : path to the linked module artifact (may be exact or a base name)
#   -D_bundle_dir    : intended destination for the assembled bundle. If missing a
#                      trailing ".xop" suffix, one will be appended.
#   -D_xop_name      : logical bundle name (used inside Info.plist & install path)
#
# Optional -D arguments:
#   -D_build_root           : top-level build root; used to look for configured Info.plist
#   -D_config               : build configuration (e.g. Release, Debug)
#   -D_configured_plist     : explicit path to an already-configured Info.plist
#   -D_r_file               : path to the macOS .r resource source file (WaveAccess.r)
#   -D_r_include_dirs       : semicolon-separated include dirs to pass to Rez (will be
#                            translated into multiple "-I dir" arguments)
#   -D_rsrc_basename        : desired basename for compiled resource (e.g. pylabhubxop64.rsrc)
#   -D_install_root         : optional post-build virtual install root; if provided,
#                            the assembled bundle will be copied under
#                            ${_install_root}/xop/${_xop_name}.xop
#                            If NOT provided, default used: <CMAKE_BINARY_DIR>/install
#
# Behavior summary:
#  - resolves the built module robustly,
#  - creates bundle Contents/MacOS and Resources,
#  - copies the module into Contents/MacOS using a bare executable name (no .so/.dylib),
#  - sets the exec bit on the copied file,
#  - attempts to run Rez to compile .r -> .rsrc using provided include dirs (if any),
#    falling back to copying the .r file if Rez isn't available or fails,
#  - writes minimal Info.plist or uses provided/configured template,
#  - writes Resources/en.lproj/InfoPlist.strings,
#  - optionally stages the assembled bundle into the post-build install root.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)

# --------------------------------------------------------------------
# Helper: sanitize a quoted/string input (strip surrounding single/double quotes
# and trim whitespace). Returns result in the specified output variable.
# --------------------------------------------------------------------
function(_strip_surrounding_quotes out_var in_var)
  if(NOT DEFINED ${in_var})
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  set(_tmp "${${in_var}}")
  string(STRIP "${_tmp}" _tmp)
  # remove surrounding single/double quotes if present
  string(REGEX REPLACE "^['\"](.*)['\"]$" "\\1" _tmp "${_tmp}")
  set(${out_var} "${_tmp}" PARENT_SCOPE)
endfunction()

# -----------------------
# Read incoming -D variables (may be quoted by generators like Xcode)
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
if(DEFINED _configured_plist)  
  set(_raw_configured_plist "${_configured_plist}") 
else() 
  set(_raw_configured_plist "") 
endif()
if(DEFINED _r_file)            
  set(_raw_r_file "${_r_file}")             
else() 
  set(_raw_r_file "") 
endif()
if(DEFINED _r_include_dirs)    
  set(_raw_r_include_dirs "${_r_include_dirs}") 
else() 
  set(_raw_r_include_dirs "") 
endif()
if(DEFINED _rsrc_basename)     
  set(_raw_rsrc_basename "${_rsrc_basename}") 
else() 
  set(_raw_rsrc_basename "") 
endif()
if(DEFINED _install_root)      
  set(_raw_install_root "${_install_root}") 
else() 
  set(_raw_install_root "") 
endif()

# sanitize inputs
_strip_surrounding_quotes(_mach_o_sanitized _raw_mach_o)
_strip_surrounding_quotes(_bundle_dir_sanitized _raw_bundle_dir)
_strip_surrounding_quotes(_xop_name_sanitized _raw_xop_name)
_strip_surrounding_quotes(_build_root_sanitized _raw_build_root)
_strip_surrounding_quotes(_config_sanitized _raw_config)
_strip_surrounding_quotes(_configured_plist_sanitized _raw_configured_plist)
_strip_surrounding_quotes(_r_file_sanitized _raw_r_file)
_strip_surrounding_quotes(_r_include_dirs_sanitized _raw_r_include_dirs)
_strip_surrounding_quotes(_rsrc_basename_sanitized _raw_rsrc_basename)
_strip_surrounding_quotes(_install_root_sanitized _raw_install_root)

message(STATUS "assemble_xop.cmake: starting assembly")
message(STATUS "  -D_mach_o           = '${_mach_o_sanitized}'")
message(STATUS "  -D_bundle_dir       = '${_bundle_dir_sanitized}'")
message(STATUS "  -D_xop_name         = '${_xop_name_sanitized}'")
if(NOT "${_build_root_sanitized}" STREQUAL "") 
  message(STATUS "  -D_build_root       = '${_build_root_sanitized}'") 
endif()
if(NOT "${_config_sanitized}" STREQUAL "")     
  message(STATUS "  -D_config           = '${_config_sanitized}'")     
endif()
if(NOT "${_configured_plist_sanitized}" STREQUAL "") 
  message(STATUS "  -D_configured_plist = '${_configured_plist_sanitized}'") 
endif()
if(NOT "${_r_file_sanitized}" STREQUAL "")     
  message(STATUS "  -D_r_file           = '${_r_file_sanitized}'")     
endif()
if(NOT "${_r_include_dirs_sanitized}" STREQUAL "") 
  message(STATUS "  -D_r_include_dirs   = '${_r_include_dirs_sanitized}'") 
endif()
if(NOT "${_rsrc_basename_sanitized}" STREQUAL "") 
  message(STATUS "  -D_rsrc_basename    = '${_rsrc_basename_sanitized}'") 
endif()
if(NOT "${_install_root_sanitized}" STREQUAL "") 
  message(STATUS "  -D_install_root     = '${_install_root_sanitized}'") 
endif()

# Validate required inputs
if(_mach_o_sanitized STREQUAL "" OR _bundle_dir_sanitized STREQUAL "" OR _xop_name_sanitized STREQUAL "")
  message(FATAL_ERROR "assemble_xop.cmake: missing required -D arguments.
Required:
  -D_mach_o
  -D_bundle_dir
  -D_xop_name
Optional:
  -D_build_root -D_config -D_configured_plist -D_r_file -D_rsrc_basename -D_install_root -D_r_include_dirs")
endif()

# -----------------------
# Resolve actual built artifact (robust to suffix differences)
# -----------------------
get_filename_component(_mach_o_abs "${_mach_o_sanitized}" ABSOLUTE)
get_filename_component(_mach_dir "${_mach_o_abs}" DIRECTORY)
get_filename_component(_mach_basename_noext "${_mach_o_abs}" NAME_WE)

message(STATUS "assemble_xop.cmake: resolving built artifact")
message(STATUS "  requested = ${_mach_o_sanitized}")
message(STATUS "  normalized= ${_mach_o_abs}")
message(STATUS "  dir       = ${_mach_dir}")
message(STATUS "  base(w/o ext) = ${_mach_basename_noext}")

# If exact path exists, use it; otherwise probe common alternative suffixes
set(_resolved_mach "")
if(EXISTS "${_mach_o_abs}")
  set(_resolved_mach "${_mach_o_abs}")
else()
  set(_alt_suffixes ".xop" ".xop.bundle" ".so" ".dylib" "")
  foreach(_s IN LISTS _alt_suffixes)
    if("${_s}" STREQUAL "")
      set(_candidate "${_mach_dir}/${_mach_basename_noext}")
    else()
      set(_candidate "${_mach_dir}/${_mach_basename_noext}${_s}")
    endif()
    if(EXISTS "${_candidate}")
      set(_resolved_mach "${_candidate}")
      message(STATUS "assemble_xop.cmake: found candidate built artifact: ${_resolved_mach}")
      break()
    endif()
  endforeach()
endif()

if(NOT _resolved_mach)
  # Helpful diagnostics for users: show listing of build directory if available
  execute_process(COMMAND /bin/ls -la "${_mach_dir}"
                  RESULT_VARIABLE _ls_res
                  OUTPUT_VARIABLE _ls_out
                  ERROR_VARIABLE _ls_err
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_ls_res EQUAL 0)
    message(STATUS "assemble_xop.cmake: directory listing:\n${_ls_out}")
  else()
    message(WARNING "assemble_xop.cmake: could not list '${_mach_dir}': ${_ls_err}")
  endif()

  message(FATAL_ERROR "assemble_xop.cmake: built target not found.
Primary: ${_mach_o_sanitized}
Normalized: ${_mach_o_abs}
Directory checked: ${_mach_dir}
Please ensure the link step produced the module and that this script runs after link.")
endif()

get_filename_component(_resolved_mach_abs "${_resolved_mach}" ABSOLUTE)
message(STATUS "assemble_xop.cmake: using resolved artifact: ${_resolved_mach_abs}")

# -----------------------
# Ensure bundle paths (append .xop if user omitted it)
# -----------------------
get_filename_component(_bundle_dir_abs "${_bundle_dir_sanitized}" ABSOLUTE)
string(REGEX MATCH "\\.xop$" _has_xop_suffix "${_bundle_dir_abs}")
if(NOT _has_xop_suffix)
  set(_bundle_dir_abs "${_bundle_dir_abs}.xop")
endif()

set(_contents_dir "${_bundle_dir_abs}/Contents")
set(_macos_dir "${_contents_dir}/MacOS")
set(_resources_dir "${_contents_dir}/Resources")
set(_en_lproj_dir "${_resources_dir}/en.lproj")

message(STATUS "assemble_xop.cmake: bundle dest = ${_bundle_dir_abs}")

# -----------------------
# If the resolved artifact is already a bundle directory, try to make idempotent
# adjustments (ensure Info.plist exists) then exit early.
# -----------------------
if(IS_DIRECTORY "${_resolved_mach_abs}")
  if(EXISTS "${_resolved_mach_abs}/Contents/MacOS")
    message(STATUS "assemble_xop.cmake: artifact is already a bundle: ${_resolved_mach_abs}")
    if(NOT EXISTS "${_resolved_mach_abs}/Contents/Info.plist")
      # Prefer explicit configured plist if provided, otherwise try build-root locations
      if(NOT "${_configured_plist_sanitized}" STREQUAL "" AND EXISTS "${_configured_plist_sanitized}")
        file(COPY "${_configured_plist_sanitized}" DESTINATION "${_resolved_mach_abs}/Contents")
        message(STATUS "assemble_xop.cmake: copied configured Info.plist -> ${_resolved_mach_abs}/Contents/Info.plist")
      else()
        message(WARNING "assemble_xop.cmake: existing bundle missing Info.plist and no configured plist provided")
      endif()
    else()
      message(STATUS "assemble_xop.cmake: bundle already contains Info.plist")
    endif()
    message(STATUS "assemble_xop.cmake: ready (existing bundle)")
    return()
  endif()
endif()

# -----------------------
# Create bundle directory structure and copy executable
# -----------------------
message(STATUS "assemble_xop.cmake: creating bundle directories")
file(MAKE_DIRECTORY "${_macos_dir}")
file(MAKE_DIRECTORY "${_resources_dir}")
file(MAKE_DIRECTORY "${_en_lproj_dir}")

# Determine dest exec name (prefer the sanitized xop name as the bare exec)
# But also attempt to strip common extension suffixes from the resolved artifact name.
get_filename_component(_resolved_mod_name "${_resolved_mach_abs}" NAME)
set(_dest_exec_name "${_resolved_mod_name}")
# strip known suffixes if present to produce a bare executable name
string(REGEX REPLACE "\\.xop$" "" _dest_exec_name "${_dest_exec_name}")
string(REGEX REPLACE "\\.xop\\.bundle$" "" _dest_exec_name "${_dest_exec_name}")
string(REGEX REPLACE "\\.dylib$|\\.so$" "" _dest_exec_name "${_dest_exec_name}")
# If the result is empty, fall back to the provided xop_name
if(_dest_exec_name STREQUAL "") 
  set(_dest_exec_name "${_xop_name_sanitized}") 
endif()

set(_dest_exec_path "${_macos_dir}/${_dest_exec_name}")

# Copy resolved module into MacOS dir using explicit target filename so the bundle
# executable name inside Contents/MacOS matches CFBundleExecutable.
# Use cmake -E copy to ensure the destination filename is as expected.
execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${_resolved_mach_abs}" "${_dest_exec_path}"
                RESULT_VARIABLE _copy_res
                OUTPUT_VARIABLE _copy_out
                ERROR_VARIABLE _copy_err
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _copy_res EQUAL 0)
  # Fall back to directory copy (should be rare); in that case derive the actual filename.
  message(WARNING "assemble_xop.cmake: copy to '${_dest_exec_path}' failed: ${_copy_err}. Falling back to directory copy.")
  file(COPY "${_resolved_mach_abs}" DESTINATION "${_macos_dir}")
  get_filename_component(_orig_name "${_resolved_mach_abs}" NAME)
  set(_dest_exec_path "${_macos_dir}/${_orig_name}")
  # attempt to normalize dest exec name for plist consistency
  get_filename_component(_dest_exec_name "${_dest_exec_path}" NAME_WE)
endif()

# Ensure executable bit on the copied file
if(EXISTS "${_dest_exec_path}")
  execute_process(COMMAND /bin/chmod +x "${_dest_exec_path}"
                  RESULT_VARIABLE _chmod_res
                  OUTPUT_VARIABLE _chmod_out
                  ERROR_VARIABLE _chmod_err
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _chmod_res EQUAL 0)
    message(WARNING "assemble_xop.cmake: could not set exec bit on ${_dest_exec_path}: ${_chmod_err}")
  else()
    message(STATUS "assemble_xop.cmake: copied module -> ${_dest_exec_path}")
  endif()
else()
  message(WARNING "assemble_xop.cmake: expected copied executable does not exist: ${_dest_exec_path}")
endif()

# -----------------------
# Info.plist discovery & installation into bundle (ordered priority)
# (same priority rules as your original version)
# -----------------------
set(_chosen_plist "")
set(_chosen_is_template OFF)

if(NOT "${_configured_plist_sanitized}" STREQUAL "" AND EXISTS "${_configured_plist_sanitized}")
  set(_chosen_plist "${_configured_plist_sanitized}")
  set(_chosen_is_template OFF)
endif()

if(_chosen_plist STREQUAL "" AND NOT "${_build_root_sanitized}" STREQUAL "")
  if(EXISTS "${_build_root_sanitized}/src/IgorXOP/Info.plist")
    set(_chosen_plist "${_build_root_sanitized}/src/IgorXOP/Info.plist")
    set(_chosen_is_template OFF)
  elseif(NOT "${_config_sanitized}" STREQUAL "" AND EXISTS "${_build_root_sanitized}/src/IgorXOP/${_config_sanitized}/Info.plist")
    set(_chosen_plist "${_build_root_sanitized}/src/IgorXOP/${_config_sanitized}/Info.plist")
    set(_chosen_is_template OFF)
  endif()
endif()

if(_chosen_plist STREQUAL "")
  if(EXISTS "${CMAKE_BINARY_DIR}/src/IgorXOP/Info.plist")
    set(_chosen_plist "${CMAKE_BINARY_DIR}/src/IgorXOP/Info.plist")
    set(_chosen_is_template OFF)
  elseif(NOT "${_config_sanitized}" STREQUAL "" AND EXISTS "${CMAKE_BINARY_DIR}/src/IgorXOP/${_config_sanitized}/Info.plist")
    set(_chosen_plist "${CMAKE_BINARY_DIR}/src/IgorXOP/${_config_sanitized}/Info.plist")
    set(_chosen_is_template OFF)
  endif()
endif()

if(_chosen_plist STREQUAL "")
  set(_tpl_candidate "${CMAKE_SOURCE_DIR}/src/IgorXOP/Info64.plist.in")
  if(EXISTS "${_tpl_candidate}")
    set(_chosen_plist "${_tpl_candidate}")
    set(_chosen_is_template ON)
  endif()
endif()

if(NOT _chosen_plist STREQUAL "")
  if(_chosen_is_template)
    configure_file("${_chosen_plist}" "${_contents_dir}/Info.plist" @ONLY)
    message(STATUS "assemble_xop.cmake: configured template -> ${_contents_dir}/Info.plist")
  else()
    file(COPY "${_chosen_plist}" DESTINATION "${_contents_dir}")
    message(STATUS "assemble_xop.cmake: copied configured Info.plist -> ${_contents_dir}/Info.plist")
  endif()
else()
  # minimal Info.plist fallback
  file(WRITE "${_contents_dir}/Info.plist"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>\n"
    "  <key>CFBundlePackageType</key><string>IXOP</string>\n"
    "  <key>CFBundleName</key><string>${_xop_name_sanitized}</string>\n"
    "  <key>CFBundleExecutable</key><string>${_dest_exec_name}</string>\n"
    "  <key>CFBundleIdentifier</key><string>edu.asu.physics.qinglab.${_xop_name_sanitized}</string>\n"
    "  <key>CFBundleShortVersionString</key><string>1.0</string>\n"
    "  <key>CFBundleVersion</key><string>1.0</string>\n"
    "  <key>CSResourcesFileMapped</key><true/>\n"
    "</dict></plist>\n")
  message(STATUS "assemble_xop.cmake: wrote minimal Info.plist -> ${_contents_dir}/Info.plist")
endif()

if(EXISTS "${_contents_dir}/Info.plist")
  message(STATUS "assemble_xop.cmake: Info.plist ready at ${_contents_dir}/Info.plist")
else()
  message(WARNING "assemble_xop.cmake: Info.plist missing after install/write")
endif()

# -----------------------
# Compile .r -> .rsrc using Rez (if available) with include dirs support
# _r_include_dirs_sanitized is a semicolon-separated list (or empty).
# -----------------------
if(NOT "${_r_file_sanitized}" STREQUAL "" AND EXISTS "${_r_file_sanitized}")
  # Prepare proper absolute output path for the rsrc
  if(NOT "${_rsrc_basename_sanitized}" STREQUAL "")
    set(_out_rsrc "${_resources_dir}/${_rsrc_basename_sanitized}")
  else()
    set(_out_rsrc "${_resources_dir}/${_xop_name_sanitized}.rsrc")
  endif()

  # Build Rez include-arguments as a CMake list so each "-I" and path is a separate arg
  set(_rez_arg_list "")
  if(NOT "${_r_include_dirs_sanitized}" STREQUAL "")
    string(REPLACE ";" " " _r_inc_space "${_r_include_dirs_sanitized}")
    foreach(_inc_dir IN LISTS _r_inc_space)
      if(NOT _inc_dir STREQUAL "")
        list(APPEND _rez_arg_list "-I" "${_inc_dir}")
      endif()
    endforeach()
  endif()

  # Find Rez executable (try common casings)
  find_program(_rez_exec Rez)
  if(NOT _rez_exec) 
    find_program(_rez_exec rez) 
  endif()

  if(_rez_exec)
    # Compose the full command list to pass to execute_process (ensures arguments are separated)
    set(_cmd_list)
    list(APPEND _cmd_list "${_rez_exec}")
    foreach(_a IN LISTS _rez_arg_list)
      list(APPEND _cmd_list "${_a}")
    endforeach()
    list(APPEND _cmd_list "-useDF" "-o" "${_out_rsrc}" "${_r_file_sanitized}")

    # For logging, show a readable single-line command
    string(REPLACE ";" " " _rez_args_readable "${_r_include_dirs_sanitized}")
    message(STATUS "assemble_xop.cmake: invoking Rez: ${_rez_exec} ${_rez_args_readable} -useDF -o ${_out_rsrc} ${_r_file_sanitized}")

    # Execute Rez
    execute_process(COMMAND ${_cmd_list}
                    RESULT_VARIABLE _rez_res
                    OUTPUT_VARIABLE _rez_out
                    ERROR_VARIABLE _rez_err
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rez_res EQUAL 0)
      message(WARNING "assemble_xop.cmake: Rez failed (${_rez_res}). Rez stderr:\n${_rez_err}\nFalling back to copying .r file into Resources/")
      file(COPY "${_r_file_sanitized}" DESTINATION "${_resources_dir}")
    else()
      message(STATUS "assemble_xop.cmake: wrote resource -> ${_out_rsrc}")
    endif()
  else()
    message(WARNING "assemble_xop.cmake: Rez not found on PATH; copying .r source into Resources/ as fallback")
    file(COPY "${_r_file_sanitized}" DESTINATION "${_resources_dir}")
  endif()
else()
  message(STATUS "assemble_xop.cmake: no .r resource file provided or not present; skipping Rez step")
endif()

# -----------------------
# Create en.lproj/InfoPlist.strings (localized strings required by some macOS versions)
# -----------------------
set(_bundle_name_val "")
set(_short_ver_val "")

if(EXISTS "${_contents_dir}/Info.plist")
  file(READ "${_contents_dir}/Info.plist" _plist_text)
  string(REGEX MATCH "<key>CFBundleName</key>[[:space:]]*<string>([^<]+)</string>" _match_name "${_plist_text}")
  if(_match_name)
    string(REGEX REPLACE ".*<key>CFBundleName</key>[[:space:]]*<string>([^<]+)</string>.*" "\\1" _bundle_name_val "${_plist_text}")
  endif()
  string(REGEX MATCH "<key>CFBundleShortVersionString</key>[[:space:]]*<string>([^<]+)</string>" _match_ver "${_plist_text}")
  if(_match_ver)
    string(REGEX REPLACE ".*<key>CFBundleShortVersionString</key>[[:space:]]*<string>([^<]+)</string>.*" "\\1" _short_ver_val "${_plist_text}")
  endif()
endif()

if(_bundle_name_val STREQUAL "") 
  set(_bundle_name_val "${_xop_name_sanitized}") 
endif()
if(_short_ver_val STREQUAL "")  
  set(_short_ver_val "1.0") 
endif()

file(WRITE "${_en_lproj_dir}/InfoPlist.strings"
  "/* Localized versions of Info.plist keys */\n\n"
  "CFBundleName = \"${_bundle_name_val}\";\n"
  "CFBundleShortVersionString = \"${_short_ver_val}\";\n"
  "CFBundleGetInfoString = \"pyLabHub XOP 64-bit.\";\n"
)
message(STATUS "assemble_xop.cmake: wrote localized InfoPlist.strings -> ${_en_lproj_dir}/InfoPlist.strings")

# -----------------------
# Post-build install staging root defaulting and bundle copy into <install_root>/xop/
# -----------------------
if("${_install_root_sanitized}" STREQUAL "")
  set(_install_root_effective "${CMAKE_BINARY_DIR}/install")
  message(STATUS "assemble_xop.cmake: no -D_install_root provided; defaulting post-build install root to '${_install_root_effective}'")
else()
  set(_install_root_effective "${_install_root_sanitized}")
  message(STATUS "assemble_xop.cmake: using provided -D_install_root = '${_install_root_effective}'")
endif()

if(NOT "${_install_root_effective}" STREQUAL "")
  set(_install_dest "${_install_root_effective}/xop")
  file(MAKE_DIRECTORY "${_install_dest}")
  execute_process(COMMAND ${CMAKE_COMMAND} -E remove_directory "${_install_dest}/${_xop_name_sanitized}.xop")
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy_directory "${_bundle_dir_abs}" "${_install_dest}/${_xop_name_sanitized}.xop"
                  RESULT_VARIABLE _copy_res
                  OUTPUT_VARIABLE _copy_out
                  ERROR_VARIABLE _copy_err
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_copy_res EQUAL 0)
    message(STATUS "assemble_xop.cmake: installed (staged) bundle -> ${_install_dest}/${_xop_name_sanitized}.xop")
  else()
    message(WARNING "assemble_xop.cmake: failed to install (stage) bundle into ${_install_dest}: ${_copy_err}")
  endif()
endif()

message(STATUS "assemble_xop.cmake: done. Bundle assembled at: ${_bundle_dir_abs}")
