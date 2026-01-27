# third_party/cmake/libsodium.cmake
#
# This script uses ExternalProject_Add to build libsodium.
# It is designed to be the first step in a prerequisite build chain.
#
# This script uses ExternalProject_Add to build libsodium.
# It is designed to be the first step in a prerequisite build chain.
#
include(ExternalProject)
include(ThirdPartyPolicyAndHelper)

# This will be set in the parent scope (third_party/CMakeLists.txt)
if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

# This will be set in the parent scope (third_party/CMakeLists.txt)
if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

set(LIBSODIUM_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libsodium")
set(LIBSODIUM_INSTALL_DIR "${PREREQ_INSTALL_DIR}") # Install to the prerequisite dir
set(LIBSODIUM_INSTALL_DIR "${PREREQ_INSTALL_DIR}") # Install to the prerequisite dir
set(LIBSODIUM_BUILD_DIR "${CMAKE_BINARY_DIR}/third_party/libsodium-build")

if(MSVC)
  # -----------------------------
  # Windows + MSVC: MSBuild solution build
  # -----------------------------
  find_program(MSBUILD_EXECUTABLE msbuild)
  if(NOT MSBUILD_EXECUTABLE)
    message(FATAL_ERROR "msbuild.exe not found. Run CMake from a Visual Studio Developer Command Prompt.")
  endif()

  # libsodium MSVC configs are typically StaticRelease/StaticDebug
  if(CMAKE_CONFIGURATION_TYPES)
    # Multi-config generator: choose config at build time with $<CONFIG>
    set(_libsodium_cfg "$<IF:$<CONFIG:Debug>,StaticDebug,StaticRelease>")
  else()
    # Single-config generator
    if(CMAKE_BUILD_TYPE MATCHES "Debug")
      set(_libsodium_cfg "StaticDebug")
    else()
      set(_libsodium_cfg "StaticRelease")
    endif()
  endif()

  if(NOT MSVC_TOOLSET_VERSION)
    message(FATAL_ERROR "MSVC_TOOLSET_VERSION is not defined, but MSVC is the compiler.")
  endif()
  message(STATUS "[libsodium.cmake] MSVC_TOOLSET_VERSION=${MSVC_TOOLSET_VERSION}")

  # Map toolset -> libsodium solution folder
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
    # Fallback to the latest known version
    set(_vs_dir "vs2022")
    message(WARNING "Unsupported Visual Studio toolset version: ${MSVC_TOOLSET_VERSION}. Falling back to vs2022. Build may fail.")
  endif()

  set(LIBSODIUM_PROJECT_ROOT_DIR "${LIBSODIUM_SOURCE_DIR}/builds/msvc/${_vs_dir}")
  set(LIBSODIUM_PROJECT_FILE "${LIBSODIUM_PROJECT_ROOT_DIR}/libsodium.sln")

  # MSBuild OutDir/IntDir need Windows-style paths and must end with a trailing slash.
  # To avoid issues with backslash parsing at the command line, we use forward
  # slashes, which MSBuild handles correctly.
  string(REPLACE "\\" "/" _out_dir_fwd "${LIBSODIUM_BUILD_DIR}/lib/")
  string(REPLACE "\\" "/" _int_dir_fwd "${LIBSODIUM_BUILD_DIR}/obj/")

  set(_msbuild_cmd
    "${MSBUILD_EXECUTABLE}" "${LIBSODIUM_PROJECT_FILE}"
    /m
    "/p:Configuration=${_libsodium_cfg}"
    "/p:Platform=${CMAKE_VS_PLATFORM_NAME}"
    "/p:PlatformToolset=${CMAKE_VS_PLATFORM_TOOLSET}"
    "/p:SolutionDir=${LIBSODIUM_PROJECT_ROOT_DIR}\\"
    "/p:OutDir=${_out_dir_fwd}"
    "/p:IntDir=${_int_dir_fwd}"
  )

  # After build, copy the produced libs and headers to INSTALL_DIR so consumers have a stable layout.
  set(_copy_cmd
    "${CMAKE_COMMAND}" -E make_directory "<INSTALL_DIR>/lib"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "<INSTALL_DIR>/include"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${LIBSODIUM_BUILD_DIR}/lib" "<INSTALL_DIR>/lib"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${LIBSODIUM_SOURCE_DIR}/src/libsodium/include" "<INSTALL_DIR>/include"
  )

  ExternalProject_Add(
    libsodium_external
    SOURCE_DIR   "${LIBSODIUM_SOURCE_DIR}"
    BINARY_DIR   "${LIBSODIUM_BUILD_DIR}"
    INSTALL_DIR  "${LIBSODIUM_INSTALL_DIR}"

    CONFIGURE_COMMAND ""   # MSBuild project is pre-generated
    INSTALL_COMMAND  ${_copy_cmd}

    # The library name produced by libsodium MSVC builds is typically "libsodium.lib"
    BUILD_BYPRODUCTS "<INSTALL_DIR>/lib/libsodium.lib"
    BUILD_COMMAND    ${_msbuild_cmd}
  )
else()
  # macOS/Linux: autotools build
  ExternalProject_Add(
    libsodium_external
    SOURCE_DIR   "${LIBSODIUM_SOURCE_DIR}"
    BINARY_DIR   "${LIBSODIUM_BUILD_DIR}"
    INSTALL_DIR  "${LIBSODIUM_INSTALL_DIR}"
    CONFIGURE_COMMAND
      "${CMAKE_COMMAND}" -E env
        "CC=${CMAKE_C_COMPILER}"
        "CXX=${CMAKE_CXX_COMPILER}"
      "${LIBSODIUM_SOURCE_DIR}/configure"
        "CC=${CMAKE_C_COMPILER}"
        "CXX=${CMAKE_CXX_COMPILER}"
      "${LIBSODIUM_SOURCE_DIR}/configure"
        --prefix=<INSTALL_DIR>
        --disable-shared
        --enable-static
        --disable-tests
        --disable-dependency-tracking
        --with-pic
    BUILD_COMMAND    "$(MAKE)"
    INSTALL_COMMAND  "$(MAKE)" install
    BUILD_BYPRODUCTS "<INSTALL_DIR>/lib/libsodium.a"
  )
endif()

message(STATUS "[pylabhub-third-party] Defined libsodium_external project to install to ${LIBSODIUM_INSTALL_DIR}")
message(STATUS "[pylabhub-third-party] Defined libsodium_external project to install to ${LIBSODIUM_INSTALL_DIR}")