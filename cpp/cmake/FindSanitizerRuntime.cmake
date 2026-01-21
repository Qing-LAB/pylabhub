# cmake/FindSanitizerRuntime.cmake
#
# Unified handler for sanitizers (Address, Thread, Undefined) for MSVC, GCC, and Clang.
#
# This script will:
# 1. Read the PYLABHUB_USE_SANITIZER cache variable.
# 2. Set the appropriate compiler and linker flags (SANITIZER_FLAGS).
# 3. For GCC/Clang, detect the sanitizer runtime library path (PYLABHUB_SANITIZER_RUNTIME_PATH).
# 4. For MSVC, stage the Address Sanitizer runtime DLLs.
# 5. Create an INTERFACE library `pylabhub::sanitizer_flags` to propagate the flags to targets.
#

string(TOLOWER "${PYLABHUB_USE_SANITIZER}" _san_name_lower)

# If no sanitizer is selected, do nothing.
if(_san_name_lower STREQUAL "none")
  set(PYLABHUB_SANITIZER_FLAGS "" CACHE STRING "Sanitizer flags" FORCE)
  return()
endif()

set(SANITIZER_FLAGS "")

# --- Handle Sanitizers by Compiler ---
if(MSVC)
  # --- MSVC Sanitizer Support ---
  if(_san_name_lower STREQUAL "address")
    message(STATUS "  ** Enabling Address Sanitizer for MSVC.")
    set(SANITIZER_FLAGS "/fsanitize=address")

    message(STATUS "  ** Setting up address sanitizer for MSVC")
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
        message(STATUS "  ** Found and will stage MSVC ASan runtime DLLs from: ${MSVC_COMPILER_DIR}")
    else()
        message(WARNING "  ** Could not find MSVC ASan runtime DLLs in the compiler directory: ${MSVC_COMPILER_DIR}. ASan-enabled tests may fail to run.")
    endif()
  else()
    message(WARNING "Sanitizer '${PYLABHUB_USE_SANITIZER}' is not supported with MSVC in this build script.")
  endif()

else()
  # --- Sanitizer Support (GCC/Clang) ---
  if(_san_name_lower STREQUAL "address")
    set(SANITIZER_FLAGS "-fsanitize=address")
  elseif(_san_name_lower STREQUAL "thread")
    set(SANITIZER_FLAGS "-fsanitize=thread")
  elseif(_san_name_lower STREQUAL "undefinedbehavior" OR _san_name_lower STREQUAL "undefined")
    set(SANITIZER_FLAGS "-fsanitize=undefined")
  endif()

  if(NOT SANITIZER_FLAGS)
     message(FATAL_ERROR "PYLABHUB_USE_SANITIZER is set to an unrecognized value: '${PYLABHUB_USE_SANITIZER}'. "
                      "Accepted values are: None, Address, Thread, UndefinedBehavior, Undefined.")
  endif()

  message(STATUS "  ** Enabling ${PYLABHUB_USE_SANITIZER} sanitizer with flags: ${SANITIZER_FLAGS}.")
  message(STATUS "")

  # detect the library for sanitizer with linker
  set(_san_lib_shortname "")
  set(_sanitizer_cmake_flag "")

  if(_san_name_lower STREQUAL "address")
    set(_san_lib_shortname "asan")
    set(_sanitizer_cmake_flag "-fsanitize=address")
  elseif(_san_name_lower STREQUAL "thread")
    set(_san_lib_shortname "tsan")
    set(_sanitizer_cmake_flag "-fsanitize=thread")
  elseif(_san_name_lower STREQUAL "undefinedbehavior" OR _san_name_lower STREQUAL "undefined")
    set(_san_lib_shortname "ubsan")
    set(_sanitizer_cmake_flag "-fsanitize=undefined")
  endif()

  set(_sanitizer_comment_name "${PYLABHUB_USE_SANITIZER}Sanitizer runtime")

  if(PLATFORM_APPLE)
    set(_suffix_regex "(dylib)")
    set(_accepted_suffixes "dylib")
  else()
    set(_suffix_regex "(a|so|dylib)")
    set(_accepted_suffixes "a;so;dylib")
  endif()

  message(STATUS "  ** PYLABHUB_USE_SANITIZER is set to ${PYLABHUB_USE_SANITIZER}")
  message(STATUS "  ** using an empty.c to find sanitizer lib path.")
  message(STATUS "  ** CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")
  message(STATUS "  ** _sanitizer_cmake_flag: ${_sanitizer_cmake_flag}")
  message(STATUS "  ** accepted suffixes: ${_accepted_suffixes}")

  # create tiny source and link with -Wl,-t to get linker trace
  file(WRITE "${CMAKE_BINARY_DIR}/empty.c" "int main() { return 0; }")
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} ${_sanitizer_cmake_flag} "-Wl,-t" "${CMAKE_BINARY_DIR}/empty.c" -o "${CMAKE_BINARY_DIR}/a.out"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE _link_result
    OUTPUT_VARIABLE _link_trace
    ERROR_VARIABLE _link_trace
  )
  file(REMOVE "${CMAKE_BINARY_DIR}/empty.c" "${CMAKE_BINARY_DIR}/a.out")
  message(STATUS "  ** _link_result: ${_link_result}")

  set(_found_path "")
  set(_found_trace_line "")

  if(_link_result EQUAL 0)
    string(REPLACE "\n" ";" _trace_lines "${_link_trace}")
    foreach(_line IN LISTS _trace_lines)
      # match either shortname (asan/tsan/ubsan) OR libclang_rt prefix + accepted suffix
      if(_line MATCHES ".*(${_san_lib_shortname}|libclang_rt).*\\.${_suffix_regex}")
        set(_found_trace_line "${_line}")
        string(REGEX MATCH "(/[^\r\n\t ]+\\.${_suffix_regex})" _path_candidate "${_line}")
        if(_path_candidate AND EXISTS "${_path_candidate}")
          set(_found_path "${_path_candidate}")
          break()
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
      set(PYLABHUB_SANITIZER_RUNTIME_TYPE "static" CACHE INTERNAL "")
    elseif(_ext STREQUAL "so" OR _ext STREQUAL "dylib")
      set(PYLABHUB_SANITIZER_RUNTIME_TYPE "shared" CACHE INTERNAL "")
    else()
      set(PYLABHUB_SANITIZER_RUNTIME_TYPE "unknown" CACHE INTERNAL "")
    endif()

    message(STATUS "  ** discovered sanitizer runtime: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
    message(STATUS "  ** runtime basename: ${PYLABHUB_SANITIZER_RUNTIME_BASENAME}")
    message(STATUS "  ** runtime type: ${PYLABHUB_SANITIZER_RUNTIME_TYPE}")
  else()
    set(PYLABHUB_SANITIZER_RUNTIME_BASENAME "" CACHE INTERNAL "")
    set(PYLABHUB_SANITIZER_RUNTIME_TYPE "unknown" CACHE INTERNAL "")
    set(PYLABHUB_SANITIZER_TRACE_LINE "" CACHE INTERNAL "")
    message(WARNING "Could not find ${_sanitizer_comment_name} via linker trace.")
  endif()

  # Stage shared runtimes
  if(PYLABHUB_SANITIZER_RUNTIME_PATH AND PYLABHUB_SANITIZER_RUNTIME_TYPE STREQUAL "shared")
    message(STATUS "Found sanitizer runtime for staging: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
    add_custom_command(TARGET create_staging_dirs POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${PYLABHUB_SANITIZER_RUNTIME_PATH}"
          "${PYLABHUB_STAGING_DIR}/lib/"
      COMMENT "Staging ${_sanitizer_comment_name} (${PYLABHUB_SANITIZER_RUNTIME_BASENAME})"
      VERBATIM
    )
  elseif(PYLABHUB_SANITIZER_RUNTIME_PATH) # static
      message(STATUS "Sanitizer runtime found but it's static (${PYLABHUB_SANITIZER_RUNTIME_BASENAME}); skipping copy to staging (not necessary).")
  else()
    message(WARNING "Could not find ${_sanitizer_comment_name}. Staged executables may not run from a clean environment.")
  endif()
  message(STATUS "")
endif() # End of if(MSVC) / else()

# --- Apply flags globally and create INTERFACE helper target ---
if(SANITIZER_FLAGS)
  set(PYLABHUB_SANITIZER_FLAGS "${SANITIZER_FLAGS}" CACHE STRING "Sanitizer flags" FORCE)

  # Append to C/CXX flags and linker flags (cache so subdirectories can read them)
  set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}"  CACHE STRING "C flags (including sanitizer)" FORCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}" CACHE STRING "CXX flags (including sanitizer)" FORCE)
  set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}" CACHE STRING "EXE linker flags (including sanitizer)" FORCE)
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${SANITIZER_FLAGS}" CACHE STRING "SHARED linker flags (including sanitizer)" FORCE)

  # Create an INTERFACE target to propagate flags
  set(_san_iface_target pylabhub_sanitizer_flags)
  if(NOT TARGET ${_san_iface_target})
    add_library(${_san_iface_target} INTERFACE)
    install(TARGETS ${_san_iface_target} EXPORT pylabhubTargets)
  endif()

  target_compile_options(${_san_iface_target} INTERFACE ${SANITIZER_FLAGS})
  if(COMMAND target_link_options)
    target_link_options(${_san_iface_target} INTERFACE ${SANITIZER_FLAGS})
  else()
    target_link_libraries(${_san_iface_target} INTERFACE "${SANITIZER_FLAGS}")
  endif()

  if(NOT TARGET pylabhub::sanitizer_flags)
    add_library(pylabhub::sanitizer_flags ALIAS ${_san_iface_target})
  endif()
endif()
