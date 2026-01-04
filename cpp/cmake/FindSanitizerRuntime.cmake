# This module is responsible for finding and staging the appropriate sanitizer runtime
# library based on the detected compiler and platform. It is included by the
# top-level CMakeLists.txt.

if(NOT MSVC AND NOT PYLABHUB_USE_SANITIZER STREQUAL "None")
  set(_sanitizer_cmake_flag "")
  set(_asan_lib_names "")
  set(_tsan_lib_names "")
  set(_ubsan_lib_names "")
  set(_sanitizer_comment_name "Sanitizer runtime") # Default comment name

  if(PYLABHUB_USE_SANITIZER STREQUAL "Address")
    set(_sanitizer_cmake_flag "-fsanitize=address")
    set(_asan_lib_names "libclang_rt.asan_osx_dynamic.dylib" "libclang_rt.asan_osx_dynamic" "libclang_rt.asan_osx" "libasan") # Prioritize specific macOS, then more general.
    set(_sanitizer_comment_name "AddressSanitizer runtime")
  elseif(PYLABHUB_USE_SANITIZER STREQUAL "Thread")
    set(_sanitizer_cmake_flag "-fsanitize=thread")
    set(_tsan_lib_names "libclang_rt.tsan_osx_dynamic" "libclang_rt.tsan_osx" "libtsan")
    set(_sanitizer_comment_name "ThreadSanitizer runtime")
  elseif(PYLABHUB_USE_SANITIZER STREQUAL "UndefinedBehavior")
    set(_sanitizer_cmake_flag "-fsanitize=undefined")
    set(_ubsan_lib_names "libclang_rt.ubsan_osx_dynamic" "libclang_rt.ubsan_osx" "libubsan")
    set(_sanitizer_comment_name "UndefinedBehaviorSanitizer runtime")
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
      OUTPUT_VARIABLE _clang_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(EXISTS "${_clang_resource_dir}")
        if(PLATFORM_APPLE)
            set(_sanitizer_search_path "${_clang_resource_dir}/lib/darwin")
        else()
            set(_sanitizer_search_path "${_clang_resource_dir}/lib")
        endif()
    endif()

    if(PYLABHUB_USE_SANITIZER STREQUAL "Address")
      find_library(_sanitizer_lib_path NAMES ${_asan_lib_names}
                   HINTS "${_sanitizer_search_path}")
      if(NOT _sanitizer_lib_path)
          find_library(_sanitizer_lib_path NAMES ${_asan_lib_names})
      endif()
    elseif(PYLABHUB_USE_SANITIZER STREQUAL "Thread")
      find_library(_sanitizer_lib_path NAMES ${_tsan_lib_names}
                   HINTS "${_sanitizer_search_path}")
      if(NOT _sanitizer_lib_path)
          find_library(_sanitizer_lib_path NAMES ${_tsan_lib_names})
      endif()
    elseif(PYLABHUB_USE_SANITIZER STREQUAL "UndefinedBehavior")
      find_library(_sanitizer_lib_path NAMES ${_ubsan_lib_names}
                   HINTS "${_sanitizer_search_path}")
      if(NOT _sanitizer_lib_path)
          find_library(_sanitizer_lib_path NAMES ${_ubsan_lib_names})
      endif()
    endif()

  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(PYLABHUB_USE_SANITIZER STREQUAL "Address")
      execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} ${_sanitizer_cmake_flag} --print-file-name=libasan.so
        OUTPUT_VARIABLE _sanitizer_lib_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
    elseif(PYLABHUB_USE_SANITIZER STREQUAL "Thread")
      execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} ${_sanitizer_cmake_flag} --print-file-name=libtsan.so
        OUTPUT_VARIABLE _sanitizer_lib_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
    elseif(PYLABHUB_USE_SANITIZER STREQUAL "UndefinedBehavior")
      execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} ${_sanitizer_cmake_flag} --print-file-name=libubsan.so
        OUTPUT_VARIABLE _sanitizer_lib_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
    endif()
  else()
    message(WARNING "Sanitizer runtime staging not supported for compiler ID: ${CMAKE_CXX_COMPILER_ID}")
  endif()

  # After finding the library path, stage it.
  if(_sanitizer_lib_path)
    # Get the actual filename for commenting purposes.
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