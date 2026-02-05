# pylabhub_apply_sanitizer_to_target
#
# Helper function to apply sanitizer flags and libraries to a given target.
# It checks if sanitizers are enabled via PYLABHUB_SANITIZER_FLAGS_SET and
# applies the INTERFACE properties from 'pylabhub::sanitizer_flags'.
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
          message(STATUS "[pylabhub-runtime-sanitize] The sanitizer is static, and PYLABHUB_LINK_STATIC_SANITIZER_INTO_SHARED_LIBS is OFF. Not linking to shared ${_target_type} ${target_name} by default.")
          message(STATUS "[pylabhub-runtime-sanitize] Please ensure your final executable that uses ${target_name} is linked to the sanitizer lib: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
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