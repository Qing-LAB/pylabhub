# cmake/consolidated_xop_assembly.cmake.in
# ------------------------------------------------------------------------------
# This script consolidates all macOS IgorXOP bundle assembly, processing,
# signing, and verification logic into a single, comprehensive script.
#
# It is executed by a POST_BUILD command on the XOP target. All necessary
# variables (both configure-time and build-time resolved generator expressions)
# are passed to it via CMake's -D mechanism during the POST_BUILD call.
#
# Expects the following variables to be defined via -D or configure_file:
#   - MACHO_BINARY_PATH: The absolute path to the compiled Mach-O binary.
#   - BUNDLE_DIR: The absolute path to the .xop bundle directory in the build tree.
#   - XOP_BUNDLE_NAME: The name of the final .xop bundle (e.g., "pylabhubxop64").
#   - CONFIGURED_INFO_PLIST: Path to the pre-configured Info.plist file.
#   - INFO_PLIST_STRINGS_SOURCE: Path to the InfoPlist.strings source file.
#   - REZ_SOURCE_FILE: Path to the Igor resource (.r) file.
#   - R_INCLUDE_DIRS_LIST: Semicolon-separated list of include directories for Rez.
#   - MACOSX_CODESIGN_IDENTITY: The identity string for code signing.
#
# The script performs the following steps:
# 1. Clean up any existing bundle directory.
# 2. Create the standard macOS bundle directory structure.
# 3. Copy and rename the Mach-O binary into the bundle.
# 4. Copy the Info.plist and InfoPlist.strings files.
# 5. Compile the .r resource file using Rez.
# 6. Clean extended attributes from the bundle (using xattr).
# 7. Code sign the bundle (using codesign).
# 8. Verify the bundle structure and contents (using nm, plutil).
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)

message(STATUS "consolidated_xop_assembly.cmake: Starting XOP bundle assembly and processing.")

# ------------------------------------------------------------------------------
# 0. Validate Inputs
# ------------------------------------------------------------------------------
if(NOT DEFINED MACHO_BINARY_PATH OR NOT DEFINED BUNDLE_DIR OR NOT DEFINED XOP_BUNDLE_NAME OR
   NOT DEFINED CONFIGURED_INFO_PLIST OR NOT DEFINED INFO_PLIST_STRINGS_SOURCE OR
   NOT DEFINED REZ_SOURCE_FILE OR NOT DEFINED R_INCLUDE_DIRS_LIST OR
   NOT DEFINED MACOSX_CODESIGN_IDENTITY)
  message(FATAL_ERROR "consolidated_xop_assembly.cmake: Required variables are missing. "
                      "Check the POST_BUILD command in src/IgorXOP/CMakeLists.txt.")
endif()

if(NOT DEFINED DEPENDENCY_DYLIB_PATH)
  set(DEPENDENCY_DYLIB_PATH "")
endif()

# Define internal paths based on provided BUNDLE_DIR
set(_contents_dir "${BUNDLE_DIR}/Contents")
set(_macos_dir    "${_contents_dir}/MacOS")
set(_resources_dir "${_contents_dir}/Resources")
set(_en_lproj_dir "${_resources_dir}/en.lproj")

# ------------------------------------------------------------------------------
# Helper Functions (moved from original separate scripts)
# ------------------------------------------------------------------------------

# --- _find_codesign_executable(OUT_VAR) ---
# Finds the Apple 'codesign' executable, preferring xcrun.
function(_find_codesign_executable out_var)
  execute_process(
    COMMAND xcrun --find codesign
    OUTPUT_VARIABLE CODESIGN_EXECUTABLE
    RESULT_VARIABLE _xcrun_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(NOT _xcrun_result EQUAL 0 OR NOT EXISTS "${CODESIGN_EXECUTABLE}")
    if(EXISTS "/usr/bin/codesign")
      set(CODESIGN_EXECUTABLE "/usr/bin/codesign")
    else()
      set(CODESIGN_EXECUTABLE)
    endif()
  endif()
  set(${out_var} "${CODESIGN_EXECUTABLE}" PARENT_SCOPE)
endfunction()

# --- _clean_extended_attributes(BUNDLE_PATH) ---
# Cleans extended attributes from a macOS bundle using xattr.
function(_clean_extended_attributes bundle_path)
  find_program(XATTR_EXECUTABLE xattr HINTS /usr/bin)
  if(XATTR_EXECUTABLE)
    message(STATUS "  - Cleaning extended attributes with xattr...")
    execute_process(COMMAND ${XATTR_EXECUTABLE} -cr "${bundle_path}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      message(WARNING "    xattr command failed with exit code ${result} for bundle: ${bundle_path}")
    endif()
  else()
    message(WARNING "  - 'xattr' tool not found. Skipping cleaning of extended attributes.")
  endif()
endfunction()

# --- _sign_bundle(BUNDLE_PATH SIGNING_IDENTITY) ---
function(_sign_bundle bundle_path signing_identity)
  # If signing_identity is empty, skip signing.
  if(signing_identity STREQUAL "")
    message(STATUS "  - Code signing skipped: no identity provided.")
    return()
  endif()

  # Find codesign
  execute_process(COMMAND xcrun --find codesign
                  OUTPUT_VARIABLE _codesign_exec
                  RESULT_VARIABLE _xcrun_find
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _xcrun_find EQUAL 0 OR NOT EXISTS "${_codesign_exec}")
    if(EXISTS "/usr/bin/codesign")
      set(_codesign_exec "/usr/bin/codesign")
    else()
      message(FATAL_ERROR "  - Code signing failed: 'codesign' not found.")
    endif()
  endif()

  message(STATUS "  - Codesign executable: ${_codesign_exec}")

  # 1) Sign nested components first (Contents/MacOS and Frameworks)
  set(_cm_path "${bundle_path}/Contents")

  # Helper: sign a single file and fail on non-zero exit
  function(_sign_one file)
    execute_process(COMMAND ${_codesign_exec} -f -s "${signing_identity}" "${file}"
                    RESULT_VARIABLE _sign_res
                    OUTPUT_QUIET ERROR_QUIET)
    if(NOT _sign_res EQUAL 0)
      message(FATAL_ERROR "    Signing failed for ${file} (exit ${_sign_res}).")
    endif()
    message(STATUS "    Signed: ${file}")
  endfunction()

  # Sign files in Contents/MacOS (executables and dylibs)
  file(GLOB_RECURSE _inner_bins RELATIVE "${_cm_path}" "${_cm_path}/MacOS/*")
  foreach(_rel IN LISTS _inner_bins)
    set(_full "${_cm_path}/${_rel}")
    if(EXISTS "${_full}")
      _sign_one("${_full}")
    endif()
  endforeach()

  # Sign frameworks (if any)
  file(GLOB _frameworks "${_cm_path}/Frameworks/*")
  foreach(_fw IN LISTS _frameworks)
    if(EXISTS "${_fw}")
      # Frameworks are directories; sign the framework directory itself.
      _sign_one("${_fw}")
    endif()
  endforeach()

  # Sign plugins or nested bundles
  file(GLOB_RECURSE _bundles RELATIVE "${_cm_path}" "${_cm_path}/*.framework" "${_cm_path}/*.bundle" "${_cm_path}/*.xpc" "${_cm_path}/*.dylib")
  foreach(_rel IN LISTS _bundles)
    set(_full "${_cm_path}/${_rel}")
    if(EXISTS "${_full}")
      _sign_one("${_full}")
    endif()
  endforeach()

  # 2) Finally sign the top-level bundle directory
  execute_process(COMMAND ${_codesign_exec} -f -s "${signing_identity}" "${bundle_path}"
                  RESULT_VARIABLE _top_res
                  OUTPUT_QUIET ERROR_QUIET)
  if(NOT _top_res EQUAL 0)
    # If signing the bundle fails, report error with hint
    message(FATAL_ERROR "    Code signing failed for bundle ${bundle_path} (exit ${_top_res}). "
                        "Ensure nested components were signed correctly and the signing identity is valid.")
  endif()
  message(STATUS "  - Bundle signed: ${bundle_path}")
endfunction()


# --- _verify_xop_bundle(BUNDLE_PATH EXECUTABLE_NAME) ---
# Verifies the structure and integrity of an XOP bundle.
function(_verify_xop_bundle bundle_path executable_name)
  message(STATUS "  - Verifying XOP bundle: ${bundle_path}")

  set(INFO_PLIST_PATH "${bundle_path}/Contents/Info.plist")
  set(EXECUTABLE_PATH "${bundle_path}/Contents/MacOS/${executable_name}")

  # 1. Check basic structure
  if(NOT EXISTS "${bundle_path}/Contents/MacOS")
    message(FATAL_ERROR "    Verification failed: Missing 'Contents/MacOS' directory in bundle.")
  endif()
  if(NOT EXISTS "${bundle_path}/Contents/Resources")
    message(FATAL_ERROR "    Verification failed: Missing 'Contents/Resources' directory in bundle.")
  endif()
  if(NOT EXISTS "${INFO_PLIST_PATH}")
    message(FATAL_ERROR "    Verification failed: Missing 'Contents/Info.plist' file in bundle.")
  endif()
  message(STATUS "    - Bundle structure................... OK")

  # 2. Check for the executable file
  if(NOT EXISTS "${EXECUTABLE_PATH}")
    message(FATAL_ERROR "    Verification failed: Missing executable file at '${EXECUTABLE_PATH}'.")
  endif()
  message(STATUS "    - Executable file.................... OK")

  # 2b. Check for the resource file
  set(RSRC_PATH "${bundle_path}/Contents/Resources/${executable_name}.rsrc")
  if(EXISTS "${REZ_SOURCE_FILE}")
    if(NOT EXISTS "${RSRC_PATH}")
      message(FATAL_ERROR "    Verification failed: Missing resource file at '${RSRC_PATH}'. "
                          "This usually means the Rez compiler failed to produce it.")
    else()
      message(STATUS "    - Resource file...................... OK")
    endif()
  endif()

  # 3. Check for exported _XOPMain symbol
  find_program(NM_EXECUTABLE nm)
  if(NOT NM_EXECUTABLE)
    message(WARNING "    - Verification warning: 'nm' tool not found. Skipping _XOPMain symbol check.")
  else()
    execute_process(
      COMMAND ${NM_EXECUTABLE} -gU "${EXECUTABLE_PATH}"
      COMMAND grep "_XOPMain"
      RESULT_VARIABLE grep_result
      OUTPUT_QUIET
      ERROR_QUIET
    )
    if(NOT grep_result EQUAL 0)
      message(FATAL_ERROR "    Verification failed: Required symbol '_XOPMain' not found or not exported in the executable.")
    endif()
    message(STATUS "    - Exported '_XOPMain' symbol....... OK")
  endif()

  # 4. Check Info.plist content (simple check for CFBundleExecutable)
  find_program(PLUTIL_EXECUTABLE plutil)
  if(NOT PLUTIL_EXECUTABLE)
    message(WARNING "    - Verification warning: 'plutil' tool not found. Skipping Info.plist content check.")
  else()
    execute_process(
      COMMAND ${PLUTIL_EXECUTABLE} -p "${INFO_PLIST_PATH}"
      COMMAND grep "CFBundleExecutable"
      RESULT_VARIABLE grep_result
      OUTPUT_VARIABLE grep_output
      ERROR_QUIET
    )
    if(NOT grep_result EQUAL 0)
      message(FATAL_ERROR "    Verification failed: Could not find 'CFBundleExecutable' key in Info.plist.")
    endif()
    if(NOT grep_output MATCHES "\"${executable_name}\"" )
      message(FATAL_ERROR "    Verification failed: 'CFBundleExecutable' key in Info.plist does not match expected executable name. Found: ${grep_output}")
    endif()
    message(STATUS "    - Info.plist CFBundleExecutable.... OK")
  endif()

  message(STATUS "  - XOP bundle verification successful.")
endfunction()

# ------------------------------------------------------------------------------
# 1. Clean up any previous bundle from the build directory
# ------------------------------------------------------------------------------
if(EXISTS "${BUNDLE_DIR}")
  message(STATUS "  - Removing old bundle: ${BUNDLE_DIR}")
  file(REMOVE_RECURSE "${BUNDLE_DIR}")
endif()

# ------------------------------------------------------------------------------
# 2. Create the standard macOS bundle directory structure
# ------------------------------------------------------------------------------
file(MAKE_DIRECTORY "${_macos_dir}")
file(MAKE_DIRECTORY "${_en_lproj_dir}")
message(STATUS "  - Created bundle directory structure in ${BUNDLE_DIR}")

# ------------------------------------------------------------------------------
# 3. Copy the compiled Mach-O binary into the bundle
# ------------------------------------------------------------------------------
message(STATUS "  - Copying binary '${MACHO_BINARY_PATH}' to '${_macos_dir}/${XOP_BUNDLE_NAME}'")
get_filename_component(source_binary_name "${MACHO_BINARY_PATH}" NAME)
set(_temp_dest_path "${_macos_dir}/${source_binary_name}")

file(COPY "${MACHO_BINARY_PATH}" DESTINATION "${_macos_dir}")
file(RENAME "${_temp_dest_path}" "${_macos_dir}/${XOP_BUNDLE_NAME}")

if(EXISTS "${_macos_dir}/${XOP_BUNDLE_NAME}")
  execute_process(COMMAND /bin/chmod +x "${_macos_dir}/${XOP_BUNDLE_NAME}")
endif()

# 3b. Copy dependency dylib if provided
if(NOT "${DEPENDENCY_DYLIB_PATH}" STREQUAL "" AND EXISTS "${DEPENDENCY_DYLIB_PATH}")
  message(STATUS "  - Copying dependency dylib '${DEPENDENCY_DYLIB_PATH}' to '${_macos_dir}/'")
  file(COPY "${DEPENDENCY_DYLIB_PATH}" DESTINATION "${_macos_dir}")
  get_filename_component(dep_name "${DEPENDENCY_DYLIB_PATH}" NAME)
  if(EXISTS "${_macos_dir}/${dep_name}")
    execute_process(COMMAND /bin/chmod +x "${_macos_dir}/${dep_name}")
  endif()
endif()

# ------------------------------------------------------------------------------
# 4. Place the Info.plist and InfoPlist.strings files
# ------------------------------------------------------------------------------
if(EXISTS "${CONFIGURED_INFO_PLIST}")
    file(COPY "${CONFIGURED_INFO_PLIST}" DESTINATION "${_contents_dir}/")
    message(STATUS "  - Copied pre-configured Info.plist to bundle.")
else()
    message(WARNING "  - CONFIGURED_INFO_PLIST was not provided. Cannot create Info.plist.")
endif()

if(EXISTS "${INFO_PLIST_STRINGS_SOURCE}")
    file(COPY "${INFO_PLIST_STRINGS_SOURCE}" DESTINATION "${_en_lproj_dir}/")
    message(STATUS "  - Copied InfoPlist.strings to 'en.lproj' resource directory.")
else()
    file(WRITE "${_en_lproj_dir}/InfoPlist.strings"
        "/* Localized versions of Info.plist keys */\n\n"
        "CFBundleName = \"${XOP_BUNDLE_NAME}\";\n"
        "CFBundleShortVersionString = \"1.0\";\n"
        "CFBundleGetInfoString = \"${XOP_BUNDLE_NAME} version 1.0\";\n"
    )
    message(STATUS "  - Wrote minimal InfoPlist.strings to bundle.")
endif()

# ------------------------------------------------------------------------------
# 5. Compile the XOP's resource file (.r -> .rsrc)
# ------------------------------------------------------------------------------
if(EXISTS "${REZ_SOURCE_FILE}")
  execute_process(
    COMMAND xcrun --find Rez
    OUTPUT_VARIABLE REZ_EXECUTABLE
    RESULT_VARIABLE REZ_FIND_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  if(REZ_FIND_RESULT EQUAL 0 AND REZ_EXECUTABLE)
    set(rez_output_rsrc "${_resources_dir}/${XOP_BUNDLE_NAME}.rsrc")
    # Build the include path arguments for Rez
    # This allows the .r file to `#include` headers from the XOP Toolkit.
    # The R_INCLUDE_DIRS_LIST variable is passed as a semicolon-separated string
    # (CMake list format) from src/IgorXOP/CMakeLists.txt. It contains paths
    # where generator expressions have already been resolved by the parent script.
    #
    # This section robustly parses this list, handles potential quoting issues,
    # and constructs the `-I` arguments for the Rez compiler.
    
    # Initialize as an empty list (NOT a quoted empty string).
    unset(rez_args)         # clears any previous value; creates an empty list variable
    set(_raw_rez_include_dirs "${R_INCLUDE_DIRS_LIST}")
    message(STATUS "  - raw R_INCLUDE_DIRS_LIST: ${_raw_rez_include_dirs}")

    # Strip outer quotes if someone injected them
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" _cleaned_rez_include_dirs "${_raw_rez_include_dirs}")
    message(STATUS "  - cleaned_rez_include_dirs: ${_cleaned_rez_include_dirs}")

    # Build -I args as separate list entries
    if(NOT "${_cleaned_rez_include_dirs}" STREQUAL "")
      foreach(_dir IN LISTS _cleaned_rez_include_dirs)
        if(NOT _dir STREQUAL "")
          get_filename_component(_dir_abs "${_dir}" ABSOLUTE)
          # Add the directory that actually contains the .r; don't add one big quoted string.
          list(APPEND rez_args "-I" "${_dir_abs}")
          # Add optional fallback (harmless if it doesn't exist)
          # list(APPEND rez_args "-I" "${_dir_abs}/XOPStandardHeaders")
        endif()
      endforeach()
    endif()

    # Show the built -I args with index to prove they are separate arguments.
    list(LENGTH rez_args _rez_arg_count)
    message(STATUS "  - rez_args has ${_rez_arg_count} elements:")
    math(EXPR _i_max "${_rez_arg_count} - 1")
    if(_rez_arg_count GREATER 0)
      foreach(_i RANGE 0 ${_i_max})
        list(GET rez_args ${_i} _val)
        message(STATUS "  - rez_args[${_i}] = '${_val}'")
      endforeach()
    else()
      message(STATUS "  - rez_args is empty (no include paths).")
    endif()

    # Build explicit command list to pass to execute_process
    set(cmd_list ${REZ_EXECUTABLE})
    # append the rez args list elements (keeps each as its own argument)
    if(_rez_arg_count GREATER 0)
      foreach(_a IN LISTS rez_args)
        list(APPEND cmd_list "${_a}")
      endforeach()
    endif()
    list(APPEND cmd_list "-useDF")
    list(APPEND cmd_list "-o" "${rez_output_rsrc}")
    list(APPEND cmd_list "${REZ_SOURCE_FILE}")

    # Show the full command to be executed (single joined string for visibility)
    string(JOIN " " _cmd_str ${cmd_list})
    message(STATUS "  - About to run Rez command: ${_cmd_str}")

    # Execute Rez and capture stdout + stderr
    execute_process(
      COMMAND ${cmd_list}
      RESULT_VARIABLE rez_result
      OUTPUT_VARIABLE rez_stdout
      ERROR_VARIABLE rez_stderr
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_STRIP_TRAILING_WHITESPACE
    )

    message(STATUS "  - Rez RESULT: ${rez_result}")
    message(STATUS "  - Rez STDOUT: ${rez_stdout}")
    message(STATUS "  - Rez STDERR: ${rez_stderr}")

    if(rez_result EQUAL 0)
      message(STATUS "  - Created resource file: ${rez_output_rsrc}")
    else()
      message(WARNING "  - Rez failed to compile resource file (exit ${rez_result}). See Rez STDERR above.")
    endif()
  endif()
else()
  message(STATUS "  - No .r file found, skipping resource compilation.")
endif()


# ------------------------------------------------------------------------------
# 6. Post-assembly Processing (Clean attributes, sign, verify)
# ------------------------------------------------------------------------------
message(STATUS "consolidated_xop_assembly.cmake: Running post-assembly processing.")
_clean_extended_attributes("${BUNDLE_DIR}")
_sign_bundle("${BUNDLE_DIR}" "${MACOSX_CODESIGN_IDENTITY}")
_verify_xop_bundle("${BUNDLE_DIR}" "${XOP_BUNDLE_NAME}")

message(STATUS "consolidated_xop_assembly.cmake: XOP bundle assembly and processing complete.")