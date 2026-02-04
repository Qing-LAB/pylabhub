# third_party/cmake/libsodium.cmake
#
# This script is a wrapper for building the libsodium library. It handles the
# major differences between the Windows and POSIX build systems for libsodium.
#
# Its strategy is to:
#  1. Detect the current platform (MSVC vs. non-MSVC).
#  2. Define the appropriate build commands:
#     - For MSVC, it uses `msbuild` to build the provided Visual Studio solution.
#     - For POSIX, it uses the standard `./configure && make` Autotools workflow.
#  3. Pass these platform-specific commands to the generic
#     `pylabhub_add_external_prerequisite` function, which handles the
#     `ExternalProject_Add` boilerplate and post-build normalization.
#
# Wrapper to build libsodium using the generic prerequisite helper function.
include(ThirdPartyPolicyAndHelper)

# --- 1. Define Paths ---
if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/libsodium")
set(_build_dir "${CMAKE_BINARY_DIR}/third_party/libsodium-build")
set(_install_dir "${PREREQ_INSTALL_DIR}")

# --- 2. Define Platform-Specific Build Commands ---
set(_configure_command "")
set(_build_command "")
set(_install_command "")
set(_byproducts "")

if(MSVC)
  # --- MSVC Build Definition ---
  find_program(_MSBUILD_EXECUTABLE msbuild REQUIRED)

  if(CMAKE_CONFIGURATION_TYPES)
    set(_libsodium_cfg "$<IF:$<CONFIG:Debug>,StaticDebug,StaticRelease>")
  else()
    if(CMAKE_BUILD_TYPE MATCHES "Debug") 
      set(_libsodium_cfg "StaticDebug")
    else() 
      set(_libsodium_cfg "StaticRelease") 
    endif()
  endif()

  if(NOT DEFINED MSVC_TOOLSET_VERSION)
    message(FATAL_ERROR "[libsodium] MSVC_TOOLSET_VERSION is not defined.")
  endif()

  set(_vs_dir "")
  if(MSVC_TOOLSET_VERSION STREQUAL "140") 
    set(_vs_dir "vs2015")
  elseif(MSVC_TOOLSET_VERSION STREQUAL "141") 
    set(_vs_dir "vs2017")
  elseif(MSVC_TOOLSET_VERSION STREQUAL "142") 
    set(_vs_dir "vs2019")
  elseif(MSVC_TOOLSET_VERSION STREQUAL "143") 
    set(_vs_dir "vs2022")
  else()
    set(_vs_dir "vs2022")
    message(WARNING "[libsodium] Unsupported MSVC_TOOLSET_VERSION: ${MSVC_TOOLSET_VERSION}. Falling back to vs2022.")
  endif()

  set(_sln_file "${_source_dir}/builds/msvc/${_vs_dir}/libsodium.sln")
  string(REPLACE "\\" "/" _out_dir_fwd "${_build_dir}/lib/")
  string(REPLACE "\\" "/" _int_dir_fwd "${_build_dir}/obj/")

  set(_build_command
    "${_MSBUILD_EXECUTABLE}" "${_sln_file}" /m
    "/p:Configuration=${_libsodium_cfg}"
    "/p:Platform=${CMAKE_VS_PLATFORM_NAME}"
    "/p:PlatformToolset=${CMAKE_VS_PLATFORM_TOOLSET}"
    "/p:SolutionDir=${_source_dir}/builds/msvc/${_vs_dir}\\"
    "/p:OutDir=${_out_dir_fwd}"
    "/p:IntDir=${_int_dir_fwd}"
  )

  # The MSBuild project for libsodium does not have an install step, so we define
  # a manual one to copy the built artifacts and headers to the install dir.
  set(_install_command
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_build_dir}/lib" "${_install_dir}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_source_dir}/src/libsodium/include" "${_install_dir}/include"
  )
  set(_byproducts "${_install_dir}/lib/libsodium.lib")

else()
  # --- POSIX (Autotools) Build Definition ---
  find_program(_make_prog make REQUIRED)
  if(DEFINED CMAKE_BUILD_PARALLEL_LEVEL)
    set(_par_args "-j${CMAKE_BUILD_PARALLEL_LEVEL}")
  else()
    set(_par_args "")
  endif()

  if(NOT EXISTS "${_source_dir}/configure")
      message(FATAL_ERROR "[libsodium] Autotools configure script not found in ${_source_dir}")
  endif()

  set(_configure_command
    ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
    "${_source_dir}/configure"
      --prefix=${_install_dir}
      --disable-shared
      --enable-static
      --disable-tests
      --disable-dependency-tracking
      --with-pic
  )
  set(_build_command ${_make_prog} ${_par_args})
  set(_install_command ${_make_prog} install)
  set(_byproducts "${_install_dir}/lib/libsodium.a")
endif()

# --- 3. Call the generic helper function ---
pylabhub_add_external_prerequisite(
  NAME              libsodium
  SOURCE_DIR        "${_source_dir}"
  BINARY_DIR        "${_build_dir}"
  INSTALL_DIR       "${_install_dir}"

  # Pass the platform-specific commands defined above
  CONFIGURE_COMMAND ${_configure_command}
  BUILD_COMMAND     ${_build_command}
  INSTALL_COMMAND   ${_install_command}
  BUILD_BYPRODUCTS  ${_byproducts}

  # Define patterns for the post-build detection script
  LIB_PATTERNS      "libsodium.lib;libsodium.a"
  HEADER_SOURCE_PATTERNS "src/libsodium/include" # Source location of headers
)

# --- 4. Provide convenience alias ---
if(NOT TARGET libsodium::pylabhub)
  add_library(libsodium::pylabhub ALIAS pylabhub::third_party::libsodium)
endif()

message(STATUS "[pylabhub-third-party] libsodium configuration complete.")

