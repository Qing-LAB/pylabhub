# ---------------------------------------------------------------------------
# third_party/cmake/libsodium.cmake
# Wrapper for libsodium.
#
# This script uses ExternalProject_Add to configure, build, and install
# libsodium into a temporary location within our build directory. It then
# creates an IMPORTED library target that can be used by other CMake targets
# within this project.
#
# Exports PYLABHUB_LIBSODIUM_ROOT_DIR for downstream consumers (e.g., libzmq).
# ---------------------------------------------------------------------------

include(ExternalProject)
include(ThirdPartyPolicyAndHelper)

set(LIBSODIUM_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libsodium")
set(LIBSODIUM_INSTALL_DIR "${CMAKE_BINARY_DIR}/third_party/libsodium-install")
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
    message(FATAL_ERROR "Unsupported Visual Studio toolset version: ${MSVC_TOOLSET_VERSION} (expected 140/141/142/143).")
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

  set(LIBSODIUM_LIBRARY_PATH "${LIBSODIUM_INSTALL_DIR}/lib/libsodium.lib")

else()
  # -----------------------------
  # macOS/Linux: autotools build
  # -----------------------------
  set(_configure_cmd "${LIBSODIUM_SOURCE_DIR}/configure")

  set(_configure_env_args
    "CC=${CMAKE_C_COMPILER}"
    "CXX=${CMAKE_CXX_COMPILER}"
  )

  if(APPLE)
    message(STATUS "[libsodium.cmake] CMAKE_OSX_DEPLOYMENT_TARGET='${CMAKE_OSX_DEPLOYMENT_TARGET}'")

    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
      message(FATAL_ERROR "[libsodium.cmake] CMAKE_OSX_DEPLOYMENT_TARGET must be set on macOS (e.g. -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0).")
    endif()

    set(_minver_flag "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    list(APPEND _configure_env_args
      "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
      "CFLAGS=${_minver_flag}"
      "CXXFLAGS=${_minver_flag}"
      "LDFLAGS=${_minver_flag}"
      # If you still see /usr/local/bin/gcc-15 being chosen, uncomment to constrain discovery:
      # "PATH=/usr/bin:/bin:/usr/sbin:/sbin"
    )
  endif()

  ExternalProject_Add(
    libsodium_external
    SOURCE_DIR   "${LIBSODIUM_SOURCE_DIR}"
    BINARY_DIR   "${LIBSODIUM_BUILD_DIR}"
    INSTALL_DIR  "${LIBSODIUM_INSTALL_DIR}"

    CONFIGURE_COMMAND
      "${CMAKE_COMMAND}" -E env
        ${_configure_env_args}
      "${_configure_cmd}"
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

  set(LIBSODIUM_LIBRARY_PATH "${LIBSODIUM_INSTALL_DIR}/lib/libsodium.a")
endif()

# ----------------------------------------------------------------------------
# Export install dir for downstream consumers
# ----------------------------------------------------------------------------
ExternalProject_Get_Property(libsodium_external install_dir)
set(PYLABHUB_LIBSODIUM_ROOT_DIR "${install_dir}"
  CACHE INTERNAL "Root directory for pylabhub's libsodium build"
)

# ----------------------------------------------------------------------------
# Imported target used by the rest of the project
# ----------------------------------------------------------------------------
add_library(pylabhub::third_party::sodium STATIC IMPORTED GLOBAL)
set_target_properties(pylabhub::third_party::sodium PROPERTIES
  IMPORTED_LOCATION "${LIBSODIUM_LIBRARY_PATH}"
  INTERFACE_INCLUDE_DIRECTORIES "${LIBSODIUM_SOURCE_DIR}/src/libsodium/include"
)

if(MSVC)
  set_property(TARGET pylabhub::third_party::sodium APPEND PROPERTY
    INTERFACE_COMPILE_DEFINITIONS "SODIUM_STATIC"
  )
endif()

add_dependencies(pylabhub::third_party::sodium libsodium_external)

message(STATUS "[pylabhub-third-party] Configured libsodium external project.")
message(STATUS "[pylabhub-third-party]   - Source:  ${LIBSODIUM_SOURCE_DIR}")
message(STATUS "[pylabhub-third-party]   - Install: ${install_dir}")
message(STATUS "[pylabhub-third-party]   - Library: ${LIBSODIUM_LIBRARY_PATH}")
message(STATUS "[pylabhub-third-party]   - Exporting libsodium root: ${PYLABHUB_LIBSODIUM_ROOT_DIR}")

if(THIRD_PARTY_INSTALL)
  message(STATUS "[pylabhub-third-party] Scheduling libsodium artifacts for staging...")

  # Stage the astatic library
  pylabhub_stage_libraries(TARGETS pylabhub::third_party::sodium)

  # Stage the header files
  pylabhub_stage_headers(
    DIRECTORIES "${install_dir}/include"
    SUBDIR ""
  )
endif()
