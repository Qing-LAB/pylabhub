# pylabhub_apply_sanitizer_to_target
#
# Helper function to apply sanitizer flags and libraries to a given target.
# It checks if sanitizers are enabled via PYLABHUB_SANITIZER_FLAGS_SET and
# applies the INTERFACE properties from 'pylabhub::sanitizer_flags'.
#
# When the sanitizer runtime is static, shared libraries (e.g. pylabhub-utils)
# do NOT link it by default to avoid duplicate symbols. Executables that load
# such shared libs must link the sanitizer themselves; use
# pylabhub_ensure_sanitizer_for_executable(<target>).
#
# Usage:
#   pylabhub_apply_sanitizer_to_target(<target_name>)
#
function(pylabhub_apply_sanitizer_to_target target_name)
  if(NOT PYLABHUB_SANITIZER_FLAGS_SET)
    return()
  endif()

  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR "pylabhub_apply_sanitizer_to_target: the target '${target_name}' does not exist. Ensure the caller passes an existing target.")
  endif()

  message(STATUS "[pylabhub-runtime-sanitize] Sanitizer detected, applying to target: ${target_name}")
  
  # Check target type to provide more context in messages
  get_target_property(_target_type ${target_name} TYPE)
  if(NOT _target_type)
    message(WARNING "  ** Could not determine type for target ${target_name}; proceeding conservatively.")
  endif()

  if(PYLABHUB_SANITIZER_RUNTIME_PATH)
    if(PYLABHUB_SANITIZER_RUNTIME_TYPE STREQUAL "shared")
      message(STATUS "[pylabhub-runtime-sanitize] The sanitizer is a dynamic lib, linking into ${_target_type} ${target_name}.")
      target_link_libraries(${target_name} PRIVATE pylabhub::sanitizer_flags)
    else() # static runtime
      if(_target_type STREQUAL "SHARED_LIBRARY" OR _target_type STREQUAL "MODULE_LIBRARY")
        if(PYLABHUB_LINK_STATIC_SANITIZER_INTO_SHARED_LIBS)
          message(STATUS "[pylabhub-runtime-sanitize] Static sanitizer: PYLABHUB_LINK_STATIC_SANITIZER_INTO_SHARED_LIBS is ON. Force linking into shared ${_target_type} ${target_name}.")
          target_link_libraries(${target_name} PRIVATE pylabhub::sanitizer_flags)
        else()
          message(STATUS "[pylabhub-runtime-sanitize] The sanitizer is static; not linking into shared ${_target_type} ${target_name} (avoids duplicate symbols when executable also links).")
          message(STATUS "[pylabhub-runtime-sanitize] Executables that use ${target_name} must link pylabhub::sanitizer_flags; call pylabhub_ensure_sanitizer_for_executable(<target>).")
        endif()
      else() # Static sanitizer into a non-shared library or executable
        message(STATUS "[pylabhub-runtime-sanitize] Static sanitizer: Linking into ${_target_type} ${target_name}.")
        target_link_libraries(${target_name} PRIVATE pylabhub::sanitizer_flags)
      endif()
    endif()
  else() # No sanitizer runtime path found
    message(STATUS "[pylabhub-runtime-sanitize] No specific sanitizer runtime path found. Linking pylabhub::sanitizer_flags to ${target_name} for compile options.")
    target_link_libraries(${target_name} PRIVATE pylabhub::sanitizer_flags)
  endif()

endfunction()

# pylabhub_ensure_sanitizer_for_executable
#
# Call this for any executable that links pylabhub-utils or other shared libs
# to which the static sanitizer is NOT linked. When the sanitizer runtime is
# static, those shared libs deliberately omit it; the executable must provide
# the sanitizer runtime for the whole process.
#
# Usage:
#   pylabhub_ensure_sanitizer_for_executable(<target_name>)
#
function(pylabhub_ensure_sanitizer_for_executable target_name)
  if(NOT PYLABHUB_SANITIZER_FLAGS_SET)
    return()
  endif()
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR "pylabhub_ensure_sanitizer_for_executable: target '${target_name}' does not exist.")
  endif()
  target_link_libraries(${target_name} PRIVATE pylabhub::sanitizer_flags)
endfunction()