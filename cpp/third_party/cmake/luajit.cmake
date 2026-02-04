# third_party/cmake/luajit.cmake
#
# This script is a wrapper for building the LuaJIT library, which uses a
# Makefile-based build system.
#
# Its strategy is to:
#  1. Determine the LuaJIT version, preferably from git tags.
#  2. Define the appropriate `make` commands for configure, build, and install.
#     The configure step includes copying the source to a separate build
#     directory to keep the source tree clean.
#  3. Pass these commands and other metadata to the generic
#     `pylabhub_add_external_prerequisite` function, which handles the
#     `ExternalProject_Add` boilerplate and post-build normalization.
#
# Wrapper to build LuaJIT using the generic prerequisite helper function.
include(ThirdPartyPolicyAndHelper)

# --- 1. Define Paths and Tools ---


set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/luajit")
set(_build_dir "${CMAKE_BINARY_DIR}/third_party/luajit-build")
set(_install_dir "${PREREQ_INSTALL_DIR}")

# Sanitize flags (strip -march etc.)
pylabhub_sanitize_compiler_flags("CMAKE_C_FLAGS" _clean_c_flags)
pylabhub_sanitize_compiler_flags("CMAKE_CXX_FLAGS" _clean_cxx_flags)

# Choose make program
if(NOT CMAKE_MAKE_PROGRAM)
  find_program(CMAKE_MAKE_PROGRAM NAMES gmake make REQUIRED)
endif()

# Parallel build args
if(DEFINED CMAKE_BUILD_PARALLEL_LEVEL AND CMAKE_BUILD_PARALLEL_LEVEL)
  set(_par_args "-j${CMAKE_BUILD_PARALLEL_LEVEL}")
else()
  set(_par_args "")
endif()

# --- 2. Determine LuaJIT version ---
set(THIRD_PARTY_LUAJIT_RELVER "" CACHE STRING "Manually specify LuaJIT RELVER (e.g., 2.1.0-beta3) if git detection fails.")
find_program(_git_executable git)
if(_git_executable AND EXISTS "${_source_dir}/.git")
  execute_process(
    COMMAND "${_git_executable}" describe --tags --always
    WORKING_DIRECTORY "${_source_dir}"
    OUTPUT_VARIABLE _luajit_detected_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_result
  )
endif()

if(NOT _git_result EQUAL 0 OR NOT _luajit_detected_version)
  if(THIRD_PARTY_LUAJIT_RELVER)
    set(_luajit_detected_version "${THIRD_PARTY_LUAJIT_RELVER}")
    message(VERBOSE "[pylabhub-third-party][luajit] Using user-provided version: ${_luajit_detected_version}")
  else()
    message(FATAL_ERROR "[pylabhub-third-party][luajit] Could not determine version from git and THIRD_PARTY_LUAJIT_RELVER is not set.")
  endif()
else()
    message(VERBOSE "[pylabhub-third-party][luajit] Detected version from git: ${_luajit_detected_version}")
endif()

# The LuaJIT build script expects a file containing just the rolling version number.
# We extract this number from the `git describe` output.
string(REGEX MATCH "-([0-9]+)-g" _match "${_luajit_detected_version}")
if(_match)
  set(_luajit_rolling_version "${CMAKE_MATCH_1}")
  message(VERBOSE "[pylabhub-third-party][luajit] Extracted rolling version number: ${_luajit_rolling_version}")
else()
  # The build script can handle this gracefully, so we default to 0.
  set(_luajit_rolling_version "0")
  message(WARNING "[pylabhub-third-party][luajit] Could not extract rolling version from '${_luajit_detected_version}'. Defaulting to 0.")
endif()


# --- 3. Define Build Commands ---

# At CONFIGURE time, create the version file in a temporary location.
set(_relver_temp_file "${CMAKE_CURRENT_BINARY_DIR}/luajit_relver.txt")
file(WRITE "${_relver_temp_file}" "${_luajit_rolling_version}\n")
message(VERBOSE "[pylabhub-third-party][luajit] Generated rolling version file at: ${_relver_temp_file}")

# Common install script path (use the same script on all platforms)
set(_install_script "${CMAKE_CURRENT_LIST_DIR}/luajit_install.cmake")

# Conditional commands for MSVC (Windows)
if(MSVC)
  # CONFIGURE_COMMAND: Copy source.
  set(_configure_command
    ${CMAKE_COMMAND} -E copy_directory "${_source_dir}" "${_build_dir}"
  )

  # BUILD_COMMAND: Run msvcbuild.bat to build LuaJIT.
  # We assume the environment is already set up with MSVC toolchain.
  # We 'cd' into the build/src directory, then run the batch file with 'static' option.
  # Re-introduce the version file copy to ensure CMake's version is used.
 
  set(_build_command
  # report action
  COMMAND ${CMAKE_COMMAND} -E echo "Cleaning .git from ${_build_dir} and ${_build_dir}/src (if present)"
  # try to remove .git if it is a file (gitfile for worktrees) - remove won't fail if it doesn't exist with -f
  COMMAND ${CMAKE_COMMAND} -E remove -f "${_build_dir}/.git"
  COMMAND ${CMAKE_COMMAND} -E remove -f "${_build_dir}/src/.git"
  # try to remove .git if it is a directory
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${_build_dir}/.git"
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${_build_dir}/src/.git"

  # copy the .relver into place (use your prepared temp file)
  COMMAND ${CMAKE_COMMAND} -E echo "Copying ${_relver_temp_file} into ${_build_dir}/src/.relver"
  COMMAND ${CMAKE_COMMAND} -E copy
    "${_relver_temp_file}"
    "${_build_dir}/src/.relver"

  # run msvcbuild.bat in the build/src directory (single cmake -E invocation)
  COMMAND ${CMAKE_COMMAND} -E echo "Running msvcbuild.bat in ${_build_dir}/src"
  COMMAND ${CMAKE_COMMAND} -E chdir "${_build_dir}/src" cmd.exe /c msvcbuild.bat static
)

  # Call the install script at build time, passing build and install directories.
  set(_install_command
    COMMAND ${CMAKE_COMMAND}
      -D "_build_dir=${_build_dir}"
      -D "_install_dir=${_install_dir}"
      -P "${_install_script}"
  )

  # Update LIB_PATTERNS and BUILD_BYPRODUCTS for MSVC.
  # Use more flexible patterns for libs and dlls.
  set(_luajit_lib_patterns "lua*.lib;lua*.dll")
  # Expect the detection script to create luajit-stable.lib from lua*.lib
  set(_luajit_build_byproduct "${_install_dir}/lib/luajit-stable.lib")
  set(_luajit_header_patterns "luajit.h;luajit_rolling.h;lua.h;lauxlib.h;lualib.h")

else() # POSIX (Linux, macOS)
  # At BUILD time, the CONFIGURE_COMMAND will copy the source code.
  set(_configure_command
    ${CMAKE_COMMAND} -E copy_directory "${_source_dir}" "${_build_dir}"
  )

  # The BUILD_COMMAND copies the version file, then runs make with a disabled
  # GIT_RELVER rule to prevent it from overwriting our file.
  # BUILD_COMMAND: clean .git, copy relver, then run make inside ${_build_dir}/src
  set(_build_command
    COMMAND ${CMAKE_COMMAND} -E echo "Cleaning .git from ${_build_dir} and ${_build_dir}/src (if present)"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${_build_dir}/.git"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${_build_dir}/src/.git"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${_build_dir}/.git"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${_build_dir}/src/.git"

    COMMAND ${CMAKE_COMMAND} -E echo "Copying relver into ${_build_dir}/src (.relver and luajit_relver.txt)"
    COMMAND ${CMAKE_COMMAND} -E copy "${_relver_temp_file}" "${_build_dir}/src/.relver"
    COMMAND ${CMAKE_COMMAND} -E copy "${_relver_temp_file}" "${_build_dir}/src/luajit_relver.txt"

    # COMMAND ${CMAKE_COMMAND} -E echo "Debug: C compiler = ${CMAKE_C_COMPILER}"
    # COMMAND ${CMAKE_COMMAND} -E echo "Debug: CFLAGS = ${_clean_c_flags}"
    # COMMAND ${CMAKE_COMMAND} -E echo "Debug: Make program = ${CMAKE_MAKE_PROGRAM}"

    COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}" "CFLAGS=${_clean_c_flags}" 
      -- ${CMAKE_MAKE_PROGRAM} -C "${_build_dir}/src" GIT_RELVER=: ${_par_args}
  )

  # Call the install script at build time, passing build and install directories.
  set(_install_command
    COMMAND ${CMAKE_COMMAND}
      -D "_build_dir=${_build_dir}"
      -D "_install_dir=${_install_dir}"
      -P "${_install_script}"
  )

  # Existing LIB_PATTERNS and BUILD_BYPRODUCTS for POSIX
  set(_luajit_lib_patterns "libluajit*.a;luajit*.a;libluajit*.so;luajit*.so;libluajit*.dylib;luajit*.dylib")
  set(_luajit_build_byproduct "${_install_dir}/lib/luajit-stable.a")
  set(_luajit_header_patterns "luajit.h;luajit_rolling.h;lua.h;lauxlib.h;lualib.h")

endif()

# --- 4. Call the generic helper function ---
pylabhub_add_external_prerequisite(
  NAME              luajit
  SOURCE_DIR        "${_source_dir}"
  BINARY_DIR        "${_build_dir}"
  INSTALL_DIR       "${_install_dir}"

  # Pass custom commands
  CONFIGURE_COMMAND ${_configure_command}
  BUILD_COMMAND     ${_build_command}
  INSTALL_COMMAND   ${_install_command}

  # # Pass patterns for the post-build detection script
  # LIB_PATTERNS      "${_luajit_lib_patterns}"
  # HEADER_SOURCE_PATTERNS "${_luajit_header_patterns}"

  # The detection script creates the stable lib, which is our byproduct
  BUILD_BYPRODUCTS  "${_luajit_build_byproduct}"

  # EXTRA_COPY_DIRECTIVES 
  #   "src/jit" "share/luajit/jit"
)

message(STATUS "[pylabhub-third-party] luajit configuration complete.")
