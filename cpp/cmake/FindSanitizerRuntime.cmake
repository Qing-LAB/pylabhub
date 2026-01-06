# This module is responsible for finding and staging the appropriate sanitizer runtime
# library based on the detected compiler and platform. It is included by the
# top-level CMakeLists.txt.

if(NOT MSVC AND NOT PYLABHUB_USE_SANITIZER STREQUAL "None")

  # Determine the name and flag for the selected sanitizer.
  string(TOLOWER "${PYLABHUB_USE_SANITIZER}" _san_name_lower)
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
    set(_sanitizer_cmake_flag "-fsanitize=undefined") # Use the correct canonical flag
  endif()

  set(_sanitizer_comment_name "${PYLABHUB_USE_SANITIZER}Sanitizer runtime")

  # Use a robust linker-trace method to find the library path.
  if(_san_lib_shortname AND _sanitizer_cmake_flag)
    if(PLATFORM_APPLE)
      set(_sanitizer_lib_suffix "dylib")
    else()
      set(_sanitizer_lib_suffix "so")
    endif()

    file(WRITE "${CMAKE_BINARY_DIR}/empty.c" "int main() { return 0; }")

    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} "${_sanitizer_cmake_flag}" "-Wl,-t" "${CMAKE_BINARY_DIR}/empty.c" -o "${CMAKE_BINARY_DIR}/a.out"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      RESULT_VARIABLE _link_result
      OUTPUT_VARIABLE _link_trace
      ERROR_VARIABLE _link_trace
    )
    file(REMOVE "${CMAKE_BINARY_DIR}/empty.c" "${CMAKE_BINARY_DIR}/a.out")

    if(_link_result EQUAL 0)
      # Split the trace output into lines and find the one containing our library.
      string(REPLACE "\n" ";" _trace_lines "${_link_trace}")
      set(_found_path "")
      foreach(_line IN LISTS _trace_lines)
        # Find a line that contains the sanitizer's short name and the correct suffix.
        # This is flexible enough to find "libasan.so" or "libclang_rt.asan_osx.dylib".
        if(_line MATCHES ".*${_san_lib_shortname}.*\\.${_sanitizer_lib_suffix}")
          # Extract the first substring that looks like an absolute file path.
          string(REGEX MATCH "([/][^ \r\n\t]+)" _path_candidate "${_line}")
          if(_path_candidate AND EXISTS "${_path_candidate}")
            set(_found_path "${_path_candidate}")
            break() # Found a valid path, stop searching.
          endif()
        endif()
      endforeach()
      set(_sanitizer_lib_path "${_found_path}")
    else()
      message(WARNING "Sanitizer '${PYLABHUB_USE_SANITIZER}' is not supported by the current compiler/platform. The '${_sanitizer_cmake_flag}' flag failed during a test compilation.")
      set(_sanitizer_lib_path "")
    endif()
  else()
      message(FATAL_ERROR "PYLABHUB_USE_SANITIZER is set to an unrecognized value: '${PYLABHUB_USE_SANITIZER}'. "
                        "Accepted values are: None, Address, Thread, UndefinedBehavior, Undefined.")
  endif()


  # After finding the library path, stage it.
  if(_sanitizer_lib_path)
    get_filename_component(_sanitizer_actual_filename "${_sanitizer_lib_path}" NAME)
    message(STATUS "Found sanitizer runtime for staging: ${_sanitizer_lib_path}")
    add_custom_command(TARGET create_staging_dirs POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_sanitizer_lib_path}"
            "${PYLABHUB_STAGING_DIR}/lib/"
        COMMENT "Staging ${_sanitizer_comment_name} (${_sanitizer_actual_filename})"
        VERBATIM
    )
  else()
    message(WARNING "Could not find ${_sanitizer_comment_name}. Staged executables may not run from a clean environment.")
  endif()
endif()