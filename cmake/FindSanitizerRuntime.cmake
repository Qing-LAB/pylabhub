# cmake/FindSanitizerRuntime.cmake
#
# Unified handler for sanitizers (Address, Thread, Undefined) for MSVC, GCC, and Clang.
#
# This script will:
# 1. Read the PYLABHUB_USE_SANITIZER cache variable.
# 2. Set the appropriate compiler and linker flags (SANITIZER_FLAGS_SET).
# 3. For GCC/Clang, detect the sanitizer runtime library path (PYLABHUB_SANITIZER_RUNTIME_PATH).
# 4. For MSVC, stage the Address Sanitizer runtime DLLs.
# 5. Create an INTERFACE library `pylabhub::sanitizer_flags` to propagate the flags to targets.
#

# ---------- Basic validation (fail early with actionable message) ----------
if(NOT DEFINED PYLABHUB_USE_SANITIZER)
  message(FATAL_ERROR "PYLABHUB_USE_SANITIZER is not defined. Set this cache variable to one of: None, Address, Thread, UndefinedBehavior, Undefined.")
endif()

if(NOT DEFINED PYLABHUB_STAGING_DIR OR "${PYLABHUB_STAGING_DIR}" STREQUAL "")
  message(FATAL_ERROR "PYLABHUB_STAGING_DIR must be defined by the consuming project before including FindSanitizerRuntime.cmake. Example: -DPYLABHUB_STAGING_DIR=${CMAKE_BINARY_DIR}/staging")
endif()

# The build system must provide the helper target 'create_staging_dirs' somewhere else.
# This file will attach POST_BUILD copy commands to that target. Fail early if it was not created.
if(NOT TARGET create_staging_dirs)
  message(FATAL_ERROR "The required helper target 'create_staging_dirs' is not defined. The top-level build must create a target named 'create_staging_dirs' before including FindSanitizerRuntime.cmake.")
endif()

# Compiler presence validation for later operations
if(NOT DEFINED CMAKE_CXX_COMPILER OR "${CMAKE_CXX_COMPILER}" STREQUAL "")
  message(FATAL_ERROR "CMAKE_CXX_COMPILER is not set. Configure a C++ compiler before enabling sanitizers.")
endif()

# ---------- Sanitizer selection ----------
string(TOLOWER "${PYLABHUB_USE_SANITIZER}" _san_name_lower)

# If no sanitizer is selected, do nothing.
if(_san_name_lower STREQUAL "none")
  set(PYLABHUB_SANITIZER_FLAGS_SET "" CACHE STRING "Sanitizer flags" FORCE)
  return()
endif()

set(SANITIZER_FLAGS_SET OFF)

# --- Handle Sanitizers by Compiler ---
if(MSVC)
  # --- MSVC Sanitizer Support ---
  if(_san_name_lower STREQUAL "address")
    message(STATUS "[pylabhub-runtime-sanitize] Enabling Address Sanitizer for MSVC.")
    set(SANITIZER_COMPILE_OPTIONS /fsanitize=address)
    set(SANITIZER_LINK_OPTIONS /fsanitize=address)
    set(SANITIZER_FLAGS_SET ON)

    message(STATUS "[pylabhub-runtime-sanitize] Setting up address sanitizer for MSVC")
    get_filename_component(MSVC_COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)

    set(ASAN_DLL_DEBUG "clang_rt.asan_dbg_dynamic-x86_64.dll")
    set(ASAN_DLL_RELEASE "clang_rt.asan_dynamic-x86_64.dll")

    # Verify that the ASan DLLs exist in the compiler's directory before staging.
    if(EXISTS "${MSVC_COMPILER_DIR}/${ASAN_DLL_DEBUG}" AND EXISTS "${MSVC_COMPILER_DIR}/${ASAN_DLL_RELEASE}")
        add_custom_command(TARGET create_staging_dirs POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSVC_COMPILER_DIR}/${ASAN_DLL_DEBUG}"
                "${PYLABHUB_STAGING_DIR}/bin/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSVC_COMPILER_DIR}/${ASAN_DLL_RELEASE}"
                "${PYLABHUB_STAGING_DIR}/bin/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSVC_COMPILER_DIR}/${ASAN_DLL_DEBUG}"
                "${PYLABHUB_STAGING_DIR}/tests/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSVC_COMPILER_DIR}/${ASAN_DLL_RELEASE}"
                "${PYLABHUB_STAGING_DIR}/tests/"
            COMMENT "Staging MSVC ASan runtime DLLs"
            VERBATIM
        )
        message(STATUS "[pylabhub-runtime-sanitize] Found and will stage MSVC ASan runtime DLLs from: ${MSVC_COMPILER_DIR}")
    else()
        message(WARNING "  ** Could not find MSVC ASan runtime DLLs in the compiler directory: ${MSVC_COMPILER_DIR}. ASan-enabled tests may fail to run.")
    endif()
  else()
    message(WARNING "Sanitizer '${PYLABHUB_USE_SANITIZER}' is not supported with MSVC in this build script.")
    set(SANITIZER_FLAGS_SET OFF)
    set(PYLABHUB_USE_SANITIZER "")
    message(STATUS "[pylabhub-runtime-sanitize] Turning sanitizer off.")
    message(STATUS "")
  endif()

else()
  # --- Sanitizer Support (GCC/Clang) ---

  # detect the library for sanitizer with linker
  set(_san_lib_shortname "")
  set(_sanitizer_cmake_flag "")

  if(_san_name_lower STREQUAL "address")
    set(SANITIZER_COMPILE_OPTIONS -fsanitize=address -fno-omit-frame-pointer)
    set(SANITIZER_LINK_OPTIONS -fsanitize=address)
    set(SANITIZER_FLAGS_SET ON)
    set(_san_lib_shortname "asan")
    set(_sanitizer_cmake_flag "-fsanitize=address")
  elseif(_san_name_lower STREQUAL "thread")
    set(SANITIZER_COMPILE_OPTIONS -fsanitize=thread -fno-omit-frame-pointer)
    set(SANITIZER_LINK_OPTIONS -fsanitize=thread)
    set(SANITIZER_FLAGS_SET ON)
    set(_san_lib_shortname "tsan")
    set(_sanitizer_cmake_flag "-fsanitize=thread")
  elseif(_san_name_lower STREQUAL "undefinedbehavior" OR _san_name_lower STREQUAL "undefined")
    set(SANITIZER_COMPILE_OPTIONS -fsanitize=undefined -fno-omit-frame-pointer)
    set(SANITIZER_LINK_OPTIONS -fsanitize=undefined)
    set(SANITIZER_FLAGS_SET ON)
    set(_san_lib_shortname "ubsan")
    set(_sanitizer_cmake_flag "-fsanitize=undefined")
  else()
    message(FATAL_ERROR "PYLABHUB_USE_SANITIZER is set to an unrecognized value: '${PYLABHUB_USE_SANITIZER}'. "
                      "Accepted values are: None, Address, Thread, UndefinedBehavior, Undefined.")
  endif()

  message(STATUS "[pylabhub-runtime-sanitize] Enabling ${PYLABHUB_USE_SANITIZER} sanitizer.")
  message(STATUS "[pylabhub-runtime-sanitize] Compile options: ${SANITIZER_COMPILE_OPTIONS}")
  message(STATUS "[pylabhub-runtime-sanitize] Link options: ${SANITIZER_LINK_OPTIONS}")
  message(STATUS "")

  set(_sanitizer_comment_name "${PYLABHUB_USE_SANITIZER}Sanitizer runtime")

  # Accept both static (.a) and shared (.so/.dylib) runtimes on all platforms.
  # The toolchain may provide either; detection and staging logic handle both correctly.
  set(_suffix_regex "(a|so|dylib)")
  set(_accepted_suffixes "a;so;dylib")

  message(STATUS "[pylabhub-runtime-sanitize] PYLABHUB_USE_SANITIZER is set to ${PYLABHUB_USE_SANITIZER}")
  message(STATUS "[pylabhub-runtime-sanitize] Using an empty.c to find sanitizer lib path.")
  message(STATUS "[pylabhub-runtime-sanitize] CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")
  message(STATUS "[pylabhub-runtime-sanitize] Sanitizer CMake flag: ${_sanitizer_cmake_flag}")
  message(STATUS "[pylabhub-runtime-sanitize] Accepted suffixes: ${_accepted_suffixes}")

  # create tiny source and link with a linker-trace flag chosen per-platform
  file(WRITE "${CMAKE_BINARY_DIR}/empty.c" "int main() { return 0; }")

  # Choose a linker trace flag conservatively. This is best-effort; if it is not supported the execute_process will fail and the script falls back.
  if(PLATFORM_APPLE)
    set(_link_flag "-Wl,-v")
  else()
    set(_link_flag "-Wl,-t")
  endif()

  # Capture stdout and stderr separately to avoid overwriting; merge for parsing below.
  set(_trace_stdout "")
  set(_trace_stderr "")
  set(_link_result -1)
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} ${_sanitizer_cmake_flag} ${_link_flag} "${CMAKE_BINARY_DIR}/empty.c" -o "${CMAKE_BINARY_DIR}/a.out"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE _link_result
    OUTPUT_VARIABLE _trace_stdout
    ERROR_VARIABLE _trace_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
  )

  # Merge outputs for parsing (preserve both)
  if(_trace_stderr)
    if(_trace_stdout)
      set(_link_trace "${_trace_stdout}\n${_trace_stderr}")
    else()
      set(_link_trace "${_trace_stderr}")
    endif()
  else()
    set(_link_trace "${_trace_stdout}")
  endif()

  file(REMOVE "${CMAKE_BINARY_DIR}/empty.c" "${CMAKE_BINARY_DIR}/a.out")
  message(STATUS "[pylabhub-runtime-sanitize] Link result: ${_link_result}")

  set(_found_path "")
  set(_found_trace_line "")

  if(_link_result EQUAL 0)
    # Normalize trace into list of lines
    string(REPLACE "\n" ";" _trace_lines "${_link_trace}")

    # Build suffix alternation for regex (convert "a;so;dylib" -> "a|so|dylib")
    string(REPLACE ";" "|" _accepted_suffix_alternation "${_accepted_suffixes}")

    foreach(_line IN LISTS _trace_lines)
      # match either shortname (asan/tsan/ubsan) OR libclang_rt / clang_rt prefixes and an accepted suffix
      if(_line MATCHES ".*(${_san_lib_shortname}|clang_rt|libclang_rt|lib${_san_lib_shortname}).*\\.${_suffix_regex}")
        set(_found_trace_line "${_line}")

        # More permissive path/basename extraction: capture tokens that end with an accepted suffix
        string(REGEX MATCH "([^ \\r\\n\\t:]+\\.(${_accepted_suffix_alternation}))" _path_candidate "${_line}")

        if(_path_candidate)
          # Try direct existence first
          if(EXISTS "${_path_candidate}")
            set(_found_path "${_path_candidate}")
            break()
          endif()

          # Try compiler directory
          get_filename_component(_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
          if(EXISTS "${_compiler_dir}/${_path_candidate}")
            set(_found_path "${_compiler_dir}/${_path_candidate}")
            break()
          endif()

          # Try a small set of common system library directories as a fallback
          foreach(_sysdir IN ITEMS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /lib /lib64)
            if(EXISTS "${_sysdir}/${_path_candidate}")
              set(_found_path "${_sysdir}/${_path_candidate}")
              break()
            endif()
          endforeach()

          if(_found_path)
            break()
          endif()
        endif()
      endif()
    endforeach()
  else()
    message(WARNING "Sanitizer test link failed (result=${_link_result}); cannot rely on linker trace to find runtime.")
  endif()

  # Expose variables for later use and for staging decision
  set(PYLABHUB_SANITIZER_RUNTIME_PATH "${_found_path}" CACHE INTERNAL "Sanitizer runtime path discovered by linker-trace")
  if(_found_path)
    get_filename_component(PYLABHUB_SANITIZER_RUNTIME_BASENAME "${_found_path}" NAME)
    set(PYLABHUB_SANITIZER_TRACE_LINE "${_found_trace_line}" CACHE INTERNAL "Linker trace line that matched sanitizer runtime")
    set(_ext "")
    foreach(_suf IN LISTS _accepted_suffixes)
      if(_found_path MATCHES "\\.${_suf}$")
        set(_ext "${_suf}")
        break()
      endif()
    endforeach()

    if(_ext STREQUAL "a")
      set(PYLABHUB_SANITIZER_RUNTIME_TYPE "static" CACHE STRING "" FORCE)
    elseif(_ext STREQUAL "so" OR _ext STREQUAL "dylib")
      set(PYLABHUB_SANITIZER_RUNTIME_TYPE "shared" CACHE STRING "" FORCE)
    else()
      set(PYLABHUB_SANITIZER_RUNTIME_TYPE "unknown" CACHE STRING "" FORCE)
    endif()

    message(STATUS "[pylabhub-runtime-sanitize] Discovered sanitizer runtime: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
    message(STATUS "[pylabhub-runtime-sanitize] Runtime basename: ${PYLABHUB_SANITIZER_RUNTIME_BASENAME}")
    message(STATUS "[pylabhub-runtime-sanitize] Runtime type: ${PYLABHUB_SANITIZER_RUNTIME_TYPE}")
  else()
    set(PYLABHUB_SANITIZER_RUNTIME_BASENAME "" CACHE STRING "" FORCE)
    set(PYLABHUB_SANITIZER_RUNTIME_TYPE "unknown" CACHE STRING "" FORCE)
    set(PYLABHUB_SANITIZER_TRACE_LINE "" CACHE STRING "" FORCE)
    set(_trace_file "${CMAKE_BINARY_DIR}/sanitizer_link_trace_${_san_lib_shortname}.txt")
    file(WRITE "${_trace_file}" "${_link_trace}")
    message(STATUS "[pylabhub-runtime-sanitize] ${_sanitizer_comment_name} path not determined via linker trace; compiler will link at build time. Trace saved to ${_trace_file}")
    if(PYLABHUB_SANITIZER_VERBOSE)
      message(STATUS "[pylabhub-runtime-sanitize] Linker trace (first 30 lines):")
      string(REPLACE "\n" ";" _trace_line_list "${_link_trace}")
      list(SUBLIST _trace_line_list 0 30 _first_lines)
      foreach(_tl IN LISTS _first_lines)
        message(STATUS "  ${_tl}")
      endforeach()
    endif()
  endif()

  # Summary: what was detected and will be used
  message(STATUS "[pylabhub-runtime-sanitize] --- Sanitizer detection summary ---")
  if(_found_path)
    message(STATUS "[pylabhub-runtime-sanitize] Sanitizer: ${PYLABHUB_USE_SANITIZER}")
    message(STATUS "[pylabhub-runtime-sanitize] Runtime:   ${PYLABHUB_SANITIZER_RUNTIME_PATH} (${PYLABHUB_SANITIZER_RUNTIME_TYPE})")
    if(PYLABHUB_SANITIZER_RUNTIME_TYPE STREQUAL "shared")
      message(STATUS "[pylabhub-runtime-sanitize] Staging:   Will copy to ${PYLABHUB_STAGING_DIR}/lib/")
    else()
      message(STATUS "[pylabhub-runtime-sanitize] Staging:   Skipped (static runtime linked into executables)")
    endif()
  else()
    message(STATUS "[pylabhub-runtime-sanitize] Sanitizer: ${PYLABHUB_USE_SANITIZER}")
    message(STATUS "[pylabhub-runtime-sanitize] Runtime:   Not found via trace; compiler driver will link at build time")
    message(STATUS "[pylabhub-runtime-sanitize] Staging:   N/A")
  endif()
  message(STATUS "[pylabhub-runtime-sanitize] ---")
  message(STATUS "")

  # Stage shared runtimes
  if(PYLABHUB_SANITIZER_RUNTIME_PATH AND PYLABHUB_SANITIZER_RUNTIME_TYPE STREQUAL "shared")
    message(STATUS "[pylabhub-runtime-sanitize] Found sanitizer runtime for staging: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
    add_custom_command(TARGET create_staging_dirs POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${PYLABHUB_SANITIZER_RUNTIME_PATH}"
          "${PYLABHUB_STAGING_DIR}/lib/"
      COMMENT "Staging ${_sanitizer_comment_name} (${PYLABHUB_SANITIZER_RUNTIME_BASENAME})"
      VERBATIM
    )

    # With shared sanitizer runtimes, executables must know where to find the .so/.dylib.
    # We set the RPATH to the staging directory's lib folder.
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    list(APPEND CMAKE_INSTALL_RPATH "${PYLABHUB_STAGING_DIR}/lib")
    message(STATUS "[pylabhub-runtime-sanitize] Adding RPATH for sanitizer: ${PYLABHUB_STAGING_DIR}/lib")
  elseif(PYLABHUB_SANITIZER_RUNTIME_PATH) # static
    message(STATUS "[pylabhub-runtime-sanitize] Sanitizer runtime is static (${PYLABHUB_SANITIZER_RUNTIME_BASENAME}); skipping copy to staging (not necessary).")
    message(STATUS "[pylabhub-runtime-sanitize] Executables that load shared libs (e.g. pylabhub-utils) must link pylabhub::sanitizer_flags; call pylabhub_ensure_sanitizer_for_executable(<target>).")
  else()
    message(STATUS "[pylabhub-runtime-sanitize] ${_sanitizer_comment_name} path not determined; compiler will link at build time.")
  endif()

  message(STATUS "")
endif() # End of if(MSVC) / else()

# --- Apply flags globally and create INTERFACE helper target ---
if(SANITIZER_FLAGS_SET)
  set(PYLABHUB_SANITIZER_FLAGS_SET "${SANITIZER_FLAGS_SET}" CACHE STRING "Sanitizer flags" FORCE)

  # Create an INTERFACE target to propagate flags
  set(_san_iface_target pylabhub_sanitizer_flags)
  
  if(NOT TARGET ${_san_iface_target})
    add_library(${_san_iface_target} INTERFACE)
    if(DEFINED SANITIZER_COMPILE_OPTIONS)
      target_compile_options(${_san_iface_target} INTERFACE ${SANITIZER_COMPILE_OPTIONS})
    endif()
    if(DEFINED SANITIZER_LINK_OPTIONS)
      target_link_options(${_san_iface_target} INTERFACE ${SANITIZER_LINK_OPTIONS})
    endif()

    install(TARGETS ${_san_iface_target} EXPORT pylabhubTargets)
  endif()

  if(NOT TARGET pylabhub::sanitizer_flags)
    add_library(pylabhub::sanitizer_flags ALIAS ${_san_iface_target})
  endif()

  string(TOUPPER "${PYLABHUB_USE_SANITIZER}" _san_name_upper)
  if(_san_name_upper STREQUAL "UNDEFINEDBEHAVIOR")
    set(_san_name_upper "UNDEFINED")
  endif()
  
  string(REGEX REPLACE "[^A-Z0-9_]" "" _san_name_upper "${_san_name_upper}")
  
  # Define a macro like PYLABHUB_SANITIZER_IS_THREAD=1
  target_compile_definitions(${_san_iface_target} INTERFACE "PYLABHUB_SANITIZER_IS_${_san_name_upper}=1")
  message(STATUS "[pylabhub-runtime-sanitize] Defining sanitizer macro for C++: PYLABHUB_SANITIZER_IS_${_san_name_upper}=1")
endif()


