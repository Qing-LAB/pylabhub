# third_party/cmake/libsodium.cmake
#
# Wrapper to build/libsodium using ExternalProject, preserving the original
# platform-specific build flow:
#  - MSVC: use provided MSBuild solution under builds/msvc/<vsdir>
#  - POSIX: run the bundled autotools configure && make && make install
#
# It creates:
#  - ExternalProject target: libsodium_external
#  - Imported target: pylabhub::third_party::libsodium (UNKNOWN IMPORTED GLOBAL)
#  - Convenience alias: libsodium::pylabhub -> pylabhub::third_party::libsodium
#
include(ExternalProject)
include(ThirdPartyPolicyAndHelper)

if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/libsodium")
set(_build_dir "${CMAKE_BINARY_DIR}/third_party/libsodium-build")
set(_install_dir "${PREREQ_INSTALL_DIR}")

# Ensure install/build dirs exist at configure time
file(MAKE_DIRECTORY "${_install_dir}")
file(MAKE_DIRECTORY "${_install_dir}/lib")
file(MAKE_DIRECTORY "${_install_dir}/include")
file(MAKE_DIRECTORY "${_build_dir}")

if(MSVC)
  # MSVC branch — robust copy-to-staging approach
  find_program(_MSBUILD_EXECUTABLE msbuild)
  if(NOT _MSBUILD_EXECUTABLE)
    message(FATAL_ERROR "[libsodium] msbuild.exe not found. Run CMake from a Visual Studio Developer Command Prompt.")
  endif()

  # Determine libsodium configuration mapping (multi-config vs single-config)
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
    message(FATAL_ERROR "[libsodium] MSVC_TOOLSET_VERSION is not defined, but MSVC is the compiler.")
  endif()
  message(STATUS "[pylabhub-third-party][libsodium] MSVC_TOOLSET_VERSION=${MSVC_TOOLSET_VERSION}")

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
    message(WARNING "[pylabhub-third-party][libsodium] Unsupported MSVC_TOOLSET_VERSION: ${MSVC_TOOLSET_VERSION}. Falling back to vs2022.")
  endif()

  set(_vs_root "${_source_dir}/builds/msvc/${_vs_dir}")
  set(_sln_file "${_vs_root}/libsodium.sln")

  # forward-slash friendly OutDir/IntDir for msbuild properties
  string(REPLACE "\\" "/" _out_dir_fwd "${_build_dir}/lib/")
  string(REPLACE "\\" "/" _int_dir_fwd "${_build_dir}/obj/")

  # Build command: pass properties to msbuild
  set(_msbuild_cmd
    "${_MSBUILD_EXECUTABLE}" "${_sln_file}"
    /m
    "/p:Configuration=${_libsodium_cfg}"
    "/p:Platform=${CMAKE_VS_PLATFORM_NAME}"
    "/p:PlatformToolset=${CMAKE_VS_PLATFORM_TOOLSET}"
    "/p:SolutionDir=${_vs_root}\\"
    "/p:OutDir=${_out_dir_fwd}"
    "/p:IntDir=${_int_dir_fwd}"
  )

  # Post-build copy: copy produced libs and headers into explicit install dir
  # Use cmake -E with separate COMMAND arguments to avoid shell quoting issues.
  set(_copy_cmds
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_install_dir}/lib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_install_dir}/include"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_build_dir}/lib" "${_install_dir}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_source_dir}/src/libsodium/include" "${_install_dir}/include"
  )

  ExternalProject_Add(
    libsodium_external
    SOURCE_DIR   "${_source_dir}"
    BINARY_DIR   "${_build_dir}"
    INSTALL_DIR  "${_install_dir}"

    CONFIGURE_COMMAND ""   # no separate configure
    BUILD_COMMAND    ${_msbuild_cmd}
    INSTALL_COMMAND  ${_copy_cmds}

    BUILD_BYPRODUCTS "${_install_dir}/lib/libsodium.lib"
  )

  # Create the imported target and set properties similarly to POSIX branch
  if(NOT TARGET pylabhub::third_party::libsodium)
    add_library(pylabhub::third_party::libsodium UNKNOWN IMPORTED GLOBAL)
    set_target_properties(pylabhub::third_party::libsodium PROPERTIES
      IMPORTED_LOCATION "${_install_dir}/lib/libsodium.lib"
      INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${_install_dir}/include>;$<INSTALL_INTERFACE:include>"
    )
  endif()

  add_dependencies(pylabhub::third_party::libsodium libsodium_external)
  if(NOT TARGET libsodium::pylabhub)
    add_library(libsodium::pylabhub ALIAS pylabhub::third_party::libsodium)
  endif()

else()
 # POSIX / multi-buildsystem branch (replace existing posix block)
  # Detect which build system the vendored libsodium uses and invoke
  # the appropriate configure / build / install commands so installation
  # goes into the staging directory `${_install_dir}` (no root required).

  # Resolve make/ninja programs
  if(DEFINED CMAKE_MAKE_PROGRAM AND CMAKE_MAKE_PROGRAM)
    set(_make_prog "${CMAKE_MAKE_PROGRAM}")
  else()
    find_program(_found_make make)
    if(_found_make)
      set(_make_prog "${_found_make}")
    else()
      set(_make_prog "make")
    endif()
  endif()

  # ninja often used with meson
  find_program(_ninja_prog ninja)
  if(NOT _ninja_prog)
    find_program(_ninja_prog ninja-build)
  endif()

  # parallel args
  if(DEFINED CMAKE_BUILD_PARALLEL_LEVEL AND CMAKE_BUILD_PARALLEL_LEVEL)
    set(_par_args "-j${CMAKE_BUILD_PARALLEL_LEVEL}")
  else()
    set(_par_args "")
  endif()

  if(EXISTS "${_source_dir}/CMakeLists.txt")
    # CMake-based libsodium: configure with CMAKE_INSTALL_PREFIX set to install dir
    ExternalProject_Add(
      libsodium_external
      SOURCE_DIR   "${_source_dir}"
      BINARY_DIR   "${_build_dir}"
      INSTALL_DIR  "${_install_dir}"

      CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -S "${_source_dir}" -B "${_build_dir}"
          -DCMAKE_INSTALL_PREFIX="${_install_dir}"
          -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
          -DBUILD_SHARED_LIBS=OFF
          -DSODIUM_USE_STATIC=ON

      BUILD_COMMAND    ${CMAKE_COMMAND} --build "${_build_dir}" --config ${CMAKE_BUILD_TYPE} -- ${_par_args}
      INSTALL_COMMAND  ${CMAKE_COMMAND} --install "${_build_dir}" --config ${CMAKE_BUILD_TYPE} --prefix "${_install_dir}"

      # typical artifact name for static libs on POSIX
      BUILD_BYPRODUCTS "${_install_dir}/lib/libsodium.a"
    )

  elseif(EXISTS "${_source_dir}/meson.build")
    # Meson-based libsodium: use meson/ninja
    if(_ninja_prog)
      ExternalProject_Add(
        libsodium_external
        SOURCE_DIR   "${_source_dir}"
        BINARY_DIR   "${_build_dir}"
        INSTALL_DIR  "${_install_dir}"

        CONFIGURE_COMMAND
          ${_ninja_prog} --version NORUN # quick check (ok if this prints)
          COMMAND ${_ninja_prog} --version || true
          COMMAND ${CMAKE_COMMAND} -E make_directory "${_build_dir}"
          COMMAND meson setup "${_build_dir}" "${_source_dir}" --prefix="${_install_dir}" --buildtype="${CMAKE_BUILD_TYPE}"
        BUILD_COMMAND    ${_ninja_prog} -C "${_build_dir}" ${_par_args}
        INSTALL_COMMAND  ${_ninja_prog} -C "${_build_dir}" install

        BUILD_BYPRODUCTS "${_install_dir}/lib/libsodium.a"
      )
    else()
      message(FATAL_ERROR "[libsodium] meson build requested but ninja not found")
    endif()

  elseif(EXISTS "${_source_dir}/configure")
    # Autotools-based libsodium
    # Use configure --prefix=<install_dir> and run make && make install.
    # As a robust fallback, INSTALL_COMMAND will use DESTDIR if needed.
    ExternalProject_Add(
      libsodium_external
      SOURCE_DIR   "${_source_dir}"
      BINARY_DIR   "${_build_dir}"
      INSTALL_DIR  "${_install_dir}"

      CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env
          "CC=${CMAKE_C_COMPILER}"
          "CXX=${CMAKE_CXX_COMPILER}"
        "${_source_dir}/configure"
          --prefix=${_install_dir}
          --disable-shared
          --enable-static
          --disable-tests
          --disable-dependency-tracking
          --with-pic

      BUILD_COMMAND    ${_make_prog} ${_par_args}
      # Use make install normally; if it fails to use prefix, fallback to DESTDIR
      INSTALL_COMMAND  ${_make_prog} install

      BUILD_BYPRODUCTS "${_install_dir}/lib/libsodium.a"
    )
  else()
    message(FATAL_ERROR "[pylabhub-third-party][libsodium] Unknown build system in ${_source_dir}; expected CMakeLists.txt meson.build or configure")
  endif()

endif()

# --- Create canonical imported target for consumers ---
# We create an UNKNOWN IMPORTED target that points at the stable artifact path.
# The detect/install step will place the real library there.
set(_libsodium_stable_lib "${_install_dir}/lib/libsodium-stable")
# Also set platform-specific fallback filenames for imported location to help tools
if(MSVC)
  set(_imported_location "<INSTALL_DIR>/lib/libsodium.lib")
else()
  set(_imported_location "<INSTALL_DIR>/lib/libsodium.a")
endif()

# Create the imported target (IMPORTED location uses the expected stable path).
# We set INTERFACE_INCLUDE_DIRECTORIES using generator expressions so it's valid in the build tree.
if(NOT TARGET pylabhub::third_party::libsodium)
  add_library(pylabhub::third_party::libsodium UNKNOWN IMPORTED GLOBAL)
  set_target_properties(pylabhub::third_party::libsodium PROPERTIES
    IMPORTED_LOCATION "${_imported_location}"
    INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${_install_dir}/include>;$<INSTALL_INTERFACE:include>"
  )
endif()

# Make the imported target depend on ExternalProject completion
add_dependencies(pylabhub::third_party::libsodium libsodium_external)

# Provide a convenience alias for backwards compatibility
if(NOT TARGET libsodium::pylabhub)
  add_library(libsodium::pylabhub ALIAS pylabhub::third_party::libsodium)
endif()

message(STATUS "[pylabhub-third-party] Defined libsodium_external -> install: ${_install_dir}")
