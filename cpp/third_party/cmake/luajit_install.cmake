# luajit_install.cmake
# Invoked with:
#   cmake -D "_build_dir=..." -D "_install_dir=..." -P luajit_install.cmake

if(NOT DEFINED _build_dir OR NOT DEFINED _install_dir)
  message(FATAL_ERROR "_build_dir and _install_dir must be defined via -D arguments.")
endif()

# Normalize paths
file(TO_CMAKE_PATH "${_build_dir}/src" _src_dir)
file(TO_CMAKE_PATH "${_src_dir}/jit" _src_jit_dir)
file(TO_CMAKE_PATH "${_install_dir}/lib" _dest_lib_dir)
file(TO_CMAKE_PATH "${_install_dir}/bin" _dest_bin_dir)
file(TO_CMAKE_PATH "${_install_dir}/include" _dest_include_dir)
file(TO_CMAKE_PATH "${_install_dir}/share/luajit/jit" _dest_jit_dir)

message(STATUS "LuaJIT install helper")
message(STATUS "  src dir: ${_src_dir}")
message(STATUS "  install dir: ${_install_dir}")

# Ensure destination directories exist
file(MAKE_DIRECTORY "${_dest_lib_dir}")
file(MAKE_DIRECTORY "${_dest_bin_dir}")
file(MAKE_DIRECTORY "${_dest_include_dir}")
file(MAKE_DIRECTORY "${_dest_jit_dir}")

# Helper macro to copy a list of absolute files to a destination (prints action)
macro(_copy_abs_files files_list dest_dir)
  foreach(_f IN LISTS ${files_list})
    if(EXISTS "${_f}" AND NOT IS_DIRECTORY "${_f}")
      message(STATUS "  Copy: ${_f} -> ${dest_dir}")
      file(COPY "${_f}" DESTINATION "${dest_dir}")
    else()
      message(VERBOSE "  Skipping (not found or directory): ${_f}")
    endif()
  endforeach()
endmacro()

# Platform-specific artifact discovery and copying
if(WIN32)
  message(STATUS "Detected: Windows (MSVC) build")

  # Executable candidates (absolute)
  file(GLOB _exe_rel "${_src_dir}/luajit*.exe")
  set(_abs_exes "")
  foreach(_e IN LISTS _exe_rel)
    # skip obvious non-executable extensions
    get_filename_component(_ename "${_e}" NAME)
    get_filename_component(_epath "${_e}" DIRECTORY)
    if(_ename MATCHES "\\.(c|h|o|txt|a|so|lib|dll)$")
      message(VERBOSE "Skipping non-exec by extension: ${_e}")
      continue()
    endif()

    if(EXISTS "${_e}" AND NOT IS_DIRECTORY "${_e}")
      # optional additional check: ensure file is executable on POSIX
      find_program(_LUAJIT_EXECUTABLE_CHECKER
             NAMES ${_ename}
             PATHS ${_epath}
             NO_DEFAULT_PATH        # Do not search standard system paths (e.g., /usr/bin, /bin)
             NO_CMAKE_FIND_ROOT_PATH # Do not search within the CMake find root path
      )
      if(_LUAJIT_EXECUTABLE_CHECKER)
        message(STATUS "executable ${_e} found.")
        list(APPEND _abs_exes "${_e}")
      else()
        message(STATUS "${_e} is not an executable, skipping...")
      endif()
    endif()
  endforeach()
  if(_abs_exes)
    _copy_abs_files(_abs_exes "${_dest_bin_dir}")
  endif()

  # Find absolute paths to libs and dlls (use GLOB to discover)
  file(GLOB _libs_abs RELATIVE "${_src_dir}" "${_src_dir}/lua*.lib" "${_src_dir}/*luajit*.lib")
  file(GLOB _dlls_abs RELATIVE "${_src_dir}" "${_src_dir}/lua*.dll" "${_src_dir}/*luajit*.dll")

  # Convert RELATIVE matches to absolute paths and copy
  set(_abs_libs "")
  foreach(_l IN LISTS _libs_abs)
    list(APPEND _abs_libs "${_src_dir}/${_l}")
  endforeach()
  if(_abs_libs)
    _copy_abs_files(_abs_libs "${_dest_lib_dir}")
  else()
    message(WARNING "No .lib files found in ${_src_dir}")
  endif()

  set(_abs_dlls "")
  foreach(_d IN LISTS _dlls_abs)
    list(APPEND _abs_dlls "${_src_dir}/${_d}")
  endforeach()
  if(_abs_dlls)
    _copy_abs_files(_abs_dlls "${_dest_lib_dir}") # keep DLLs in lib/ per your staging flow
  endif()

else()
  message(STATUS "Detected: POSIX/Unix build")

  # Executable candidates (absolute)
  file(GLOB _exe_rel "${_src_dir}/luajit" "${_src_dir}/luajit*")
  set(_abs_exes "")
  foreach(_e IN LISTS _exe_rel)
    # skip obvious non-executable extensions
    get_filename_component(_ename "${_e}" NAME)
    get_filename_component(_epath "${_e}" DIRECTORY)
    if(_ename MATCHES "\\.(c|h|o|txt|a|so|lib|dll)$")
      message(VERBOSE "Skipping non-exec by extension: ${_e}")
      continue()
    endif()

    if(EXISTS "${_e}" AND NOT IS_DIRECTORY "${_e}")
      # optional additional check: ensure file is executable on POSIX
      find_program(_LUAJIT_EXECUTABLE_CHECKER
             NAMES ${_ename}
             PATHS ${_epath}
             NO_DEFAULT_PATH        # Do not search standard system paths (e.g., /usr/bin, /bin)
             NO_CMAKE_FIND_ROOT_PATH # Do not search within the CMake find root path
      )
      if(_LUAJIT_EXECUTABLE_CHECKER)
        message(STATUS "executable ${_e} found.")
        list(APPEND _abs_exes "${_e}")
      else()
        message(STATUS "${_e} is not an executable, skipping...")
      endif()
    endif()
  endforeach()
  if(_abs_exes)
    _copy_abs_files(_abs_exes "${_dest_bin_dir}")
  endif()

  # Static libs
  file(GLOB _static_libs "${_src_dir}/libluajit*.a" "${_src_dir}/lua*.a")
  if(_static_libs)
    _copy_abs_files(_static_libs "${_dest_lib_dir}")
  endif()

  # Shared libs
  file(GLOB _shared_libs "${_src_dir}/libluajit*.so" "${_src_dir}/liblua*.so" "${_src_dir}/libluajit*.dylib" "${_src_dir}/liblua*.dylib")
  if(_shared_libs)
    _copy_abs_files(_shared_libs "${_dest_lib_dir}")
  endif()
endif()

# Headers (guarded)
set(_header_names
  "lua.h"
  "luajit.h"
  "lauxlib.h"
  "lualib.h"
)

set(_found_headers "")
foreach(_h IN LISTS _header_names)
  if(EXISTS "${_src_dir}/${_h}")
    list(APPEND _found_headers "${_src_dir}/${_h}")
  else()
    message(WARNING "Header not found: ${_src_dir}/${_h}")
  endif()
endforeach()
if(_found_headers)
  _copy_abs_files(_found_headers "${_dest_include_dir}")
else()
  message(WARNING "No LuaJIT headers found in ${_src_dir}; check build output.")
endif()

# JIT runtime files
if(EXISTS "${_src_jit_dir}" AND IS_DIRECTORY "${_src_jit_dir}")
  message(STATUS "Copying JIT runtime files from ${_src_jit_dir} -> ${_dest_jit_dir}")
  file(COPY "${_src_jit_dir}/" DESTINATION "${_dest_jit_dir}")
else()
  message(STATUS "No JIT runtime directory at ${_src_jit_dir}; skipping.")
endif()

# Final check: warn if nothing was installed
if(NOT _abs_exes AND NOT _static_libs AND NOT _shared_libs AND NOT _found_headers)
  message(WARNING "LuaJIT install step completed but no artifacts were copied. Check the build output.")
endif()

message(STATUS "LuaJIT install helper finished.")
