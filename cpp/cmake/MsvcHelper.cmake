# cmake/MsvcHelper.cmake
#
# Provides helper functions for applying common MSVC-specific options to targets.
#
# Usage:
#   pylabhub_apply_standard_msvc_options(<target_name>)
#
function(pylabhub_apply_standard_msvc_options target_name)
  if(MSVC)
    # Enable synchronous exception handling and disable warning C5105.
    target_compile_options(${target_name} PRIVATE /EHsc /wd5105)
    # Enable the standards-conforming preprocessor
    target_compile_options(${target_name} PRIVATE /Zc:preprocessor)
  endif()
endfunction()