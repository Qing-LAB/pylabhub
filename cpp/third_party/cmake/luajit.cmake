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
if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/luajit")
set(_build_dir "${CMAKE_BINARY_DIR}/third_party/luajit-build")
set(_install_dir "${PREREQ_INSTALL_DIR}")

# Sanitize flags (strip -march etc.)
pylabhub_sanitize_compiler_flags("CMAKE_C_FLAGS" _clean_c_flags)

# Choose make program
if(DEFINED CMAKE_MAKE_PROGRAM AND CMAKE_MAKE_PROGRAM)
  set(_make_prog "${CMAKE_MAKE_PROGRAM}")
else()
  find_program(_make_prog make REQUIRED)
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
    message(STATUS "[pylabhub-third-party][luajit] Using user-provided version: ${_luajit_detected_version}")
  else()
    message(FATAL_ERROR "[pylabhub-third-party][luajit] Could not determine version from git and THIRD_PARTY_LUAJIT_RELVER is not set.")
  endif()
else()
    message(STATUS "[pylabhub-third-party][luajit] Detected version from git: ${_luajit_detected_version}")
endif()

# The LuaJIT build script expects a file containing just the rolling version number.
# We extract this number from the `git describe` output.
string(REGEX MATCH "-([0-9]+)-g" _match "${_luajit_detected_version}")
if(_match)
  set(_luajit_rolling_version "${CMAKE_MATCH_1}")
  message(STATUS "[pylabhub-third-party][luajit] Extracted rolling version number: ${_luajit_rolling_version}")
else()
  # The build script can handle this gracefully, so we default to 0.
  set(_luajit_rolling_version "0")
  message(WARNING "[pylabhub-third-party][luajit] Could not extract rolling version from '${_luajit_detected_version}'. Defaulting to 0.")
endif()


# --- 3. Define Build Commands for Makefile-based project ---

# At CONFIGURE time, create the version file in a temporary location.
set(_relver_temp_file "${CMAKE_CURRENT_BINARY_DIR}/luajit_relver.txt")
file(WRITE "${_relver_temp_file}" "${_luajit_rolling_version}\n")
message(STATUS "[pylabhub-third-party][luajit] Generated rolling version file at: ${_relver_temp_file}")

# At BUILD time, the CONFIGURE_COMMAND will copy the source code.
set(_configure_command
  ${CMAKE_COMMAND} -E copy_directory "${_source_dir}" "${_build_dir}"
)

# The BUILD_COMMAND copies the version file, then runs make with a disabled
# GIT_RELVER rule to prevent it from overwriting our file.
set(_build_command
  ${CMAKE_COMMAND} -E copy
    "${_relver_temp_file}"
    "${_build_dir}/src/luajit_relver.txt"
  COMMAND ${CMAKE_COMMAND} -E env
    "CC=${CMAKE_C_COMPILER}"
    "CFLAGS=${_clean_c_flags}"
    ${_make_prog} -C "${_build_dir}/src" "GIT_RELVER=:" ${_par_args}
)

set(_install_command
  ${CMAKE_COMMAND} -E env
    "CC=${CMAKE_C_COMPILER}"
    "CFLAGS=${_clean_c_flags}"
    ${_make_prog} -C "${_build_dir}" install "GIT_RELVER=:" PREFIX=${_install_dir}
)
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

  # Pass patterns for the post-build detection script
  LIB_PATTERNS      "libluajit*.a;luajit*.a;libluajit*.so;luajit*.so;libluajit*.dylib;luajit*.dylib"
  HEADER_SOURCE_PATTERNS "include/luajit-*"

  # The detection script creates the stable lib, which is our byproduct
  BUILD_BYPRODUCTS  "${_install_dir}/lib/luajit-stable.a"
)

# --- 5. Provide convenience alias ---
if(NOT TARGET luajit::pylabhub)
  add_library(luajit::pylabhub ALIAS pylabhub::third_party::luajit)
endif()

message(STATUS "[pylabhub-third-party] luajit configuration complete.")

