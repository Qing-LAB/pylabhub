# --------------------------------------------------------------------------- 
# third_party/cmake/libsodium.cmake
# Wrapper for libsodium.
#
# This script uses ExternalProject_Add to configure, build, and install
# libsodium into a temporary location within our build directory. It then
# creates an IMPORTED library target that can be used by other CMake targets
# within this project.
#
# To support dependent projects like libzmq, it exports the installation
# directory path to the `PYLABHUB_LIBSODIUM_ROOT_DIR` cache variable.
# --------------------------------------------------------------------------- 
include(ExternalProject)
include(ThirdPartyPolicyAndHelper)

set(LIBSODIUM_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libsodium")
set(LIBSODIUM_INSTALL_DIR "${CMAKE_BINARY_DIR}/third_party/libsodium-install")
set(LIBSODIUM_BUILD_DIR "${CMAKE_BINARY_DIR}/third_party/libsodium-build")

if(MSVC)
    # On Windows with MSVC, build using MSBuild.
    find_program(MSBUILD_EXECUTABLE msbuild)
    if(NOT MSBUILD_EXECUTABLE)
        message(FATAL_ERROR "msbuild.exe not found. Please run cmake from a developer command prompt.")
    endif()

    if(CMAKE_BUILD_TYPE MATCHES "Debug")
      set(LIBSODIUM_MSBUILD_CONFIG "StaticDebug")
    else()
      set(LIBSODIUM_MSBUILD_CONFIG "StaticRelease")
    endif()

    # Determine the correct vcxproj path based on the Visual Studio toolset version
    if(NOT MSVC_TOOLSET_VERSION)
      message(FATAL_ERROR "MSVC_TOOLSET_VERSION is not defined, but MSVC is the compiler. This should not happen.")
    endif()
    set(CURRENT_VS_TOOLSET_VERSION ${MSVC_TOOLSET_VERSION})
    message(STATUS "[libsodium.cmake] MSVC_TOOLSET_VERSION = ${MSVC_TOOLSET_VERSION}")

    set(LIBSODIUM_VS_VERSION_MAP
        "140=vs2015"
        "141=vs2017"
        "142=vs2019"
        "143=vs2022"
        "145=vs2026" # adjust if needed
    )

    set(LIBSODIUM_VS_DIR "")
    foreach(item ${LIBSODIUM_VS_VERSION_MAP})
        string(REPLACE "=" ";" pair ${item})
        list(GET pair 0 key)
        list(GET pair 1 value)
        if("${key}" STREQUAL "${CURRENT_VS_TOOLSET_VERSION}")
            set(LIBSODIUM_VS_DIR "${value}")
            break()
        endif()
    endforeach()

    if(NOT LIBSODIUM_VS_DIR)
        message(FATAL_ERROR "Unsupported Visual Studio toolset version: ${MSVC_TOOLSET_VERSION}. Supported versions: ${LIBSODIUM_VS_VERSION_MAP}")
    endif()

    set(LIBSODIUM_PROJECT_ROOT_DIR "${LIBSODIUM_SOURCE_DIR}/builds/msvc/${LIBSODIUM_VS_DIR}")
    set(LIBSODIUM_PROJECT_FILE "${LIBSODIUM_PROJECT_ROOT_DIR}/libsodium.sln")

    file(TO_CMAKE_PATH "${LIBSODIUM_BUILD_DIR}/lib" out_dir_path)
    file(TO_CMAKE_PATH "${LIBSODIUM_BUILD_DIR}/obj" int_dir_path)

    set(LIBSODIUM_BUILD_CMD
        ${MSBUILD_EXECUTABLE}
        ${LIBSODIUM_PROJECT_FILE}
        /p:Configuration=${LIBSODIUM_MSBUILD_CONFIG}
        /p:Platform=${CMAKE_VS_PLATFORM_NAME}
        /p:PlatformToolset=${CMAKE_VS_PLATFORM_TOOLSET}
        "/p:SolutionDir=${LIBSODIUM_PROJECT_ROOT_DIR}\\"
        "/p:OutDir=${out_dir_path}/"
        "/p:IntDir=${int_dir_path}/"
    )

    ExternalProject_Add(
      libsodium_external
      SOURCE_DIR ${LIBSODIUM_SOURCE_DIR}
      INSTALL_DIR ${LIBSODIUM_INSTALL_DIR}
      BINARY_DIR ${LIBSODIUM_BUILD_DIR}
      CONFIGURE_COMMAND ""
      INSTALL_COMMAND ""
      BUILD_COMMAND ${LIBSODIUM_BUILD_CMD}
    )
    set(LIBSODIUM_LIBRARY_PATH "${LIBSODIUM_INSTALL_DIR}/lib/libsodium.lib")
else()
    # For other platforms (Linux, macOS, MinGW), use the existing autotools build.
    find_package(Git REQUIRED)
    if(WIN32) # Non-MSVC on Windows (MinGW/MSYS)
        find_program(SH_EXECUTABLE NAMES sh.exe PATHS "C:/Program Files/Git/usr/bin" "C:/Program Files (x86)/Git/usr/bin" ENV PATH)
        if(NOT SH_EXECUTABLE)
            message(FATAL_ERROR "sh.exe not found. Please install Git for Windows and ensure its bin directory is in your PATH.")
        endif()
        set(CONFIGURE_COMMAND ${SH_EXECUTABLE} ${LIBSODIUM_SOURCE_DIR}/configure)
    else() # Linux, macOS
        set(CONFIGURE_COMMAND ${LIBSODIUM_SOURCE_DIR}/configure)
    endif()

    ExternalProject_Add(
      libsodium_external
      SOURCE_DIR ${LIBSODIUM_SOURCE_DIR}
      INSTALL_DIR ${LIBSODIUM_INSTALL_DIR}
      BINARY_DIR ${LIBSODIUM_BUILD_DIR}
      CONFIGURE_COMMAND ${CONFIGURE_COMMAND}
          --prefix=${LIBSODIUM_INSTALL_DIR}
          --disable-shared
          --enable-static
          --disable-tests
          --disable-dependency-tracking
          --with-pic
      BUILD_COMMAND $(MAKE)
      INSTALL_COMMAND $(MAKE) install
      BUILD_BYPRODUCTS "${LIBSODIUM_INSTALL_DIR}/lib/libsodium.a"
    )
    set(LIBSODIUM_LIBRARY_PATH "${LIBSODIUM_INSTALL_DIR}/lib/libsodium.a")
endif()

if(MSVC)
  add_custom_command(TARGET libsodium_external POST_BUILD
      COMMENT "Copying libsodium artifacts post-build"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${LIBSODIUM_BUILD_DIR}/lib" "${LIBSODIUM_INSTALL_DIR}/lib"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${LIBSODIUM_SOURCE_DIR}/src/libsodium/include" "${LIBSODIUM_INSTALL_DIR}/include"
  )
endif()

# Use ExternalProject_Get_Property to get the definitive install directory.
# This path is then exported as a cache variable to be used as a hint for
# find_package(Sodium) in other projects (like libzmq).
ExternalProject_Get_Property(libsodium_external install_dir)
set(PYLABHUB_LIBSODIUM_ROOT_DIR ${install_dir}
    CACHE INTERNAL "Root directory for pylabhub's libsodium build"
)

# Create an IMPORTED library target. This is what other targets will link against.
add_library(pylabhub::third_party::sodium STATIC IMPORTED GLOBAL)
set_target_properties(pylabhub::third_party::sodium PROPERTIES
  IMPORTED_LOCATION "${LIBSODIUM_LIBRARY_PATH}"
  INTERFACE_INCLUDE_DIRECTORIES "${install_dir}/include"
)

if(MSVC)
    set_property(TARGET pylabhub::third_party::sodium APPEND PROPERTY
        INTERFACE_COMPILE_DEFINITIONS "SODIUM_STATIC"
    )
endif()

# Make sure the external project is built before this target is used.
add_dependencies(pylabhub::third_party::sodium libsodium_external)

message(STATUS "[pylabhub-third-party] Configured libsodium external project.")
message(STATUS "[pylabhub-third-party]   - Source: ${LIBSODIUM_SOURCE_DIR}")
message(STATUS "[pylabhub-third-party]   - Install: ${install_dir}")
message(STATUS "[pylabhub-third-party]   - Library: ${LIBSODIUM_LIBRARY_PATH}")
message(STATUS "[pylabhub-third-party]   - Exporting libsodium root for other projects: ${PYLABHUB_LIBSODIUM_ROOT_DIR}")

if(THIRD_PARTY_INSTALL)
  message(STATUS "[pylabhub-third-party] Scheduling libsodium artifacts for staging...")

  # Stage the astatic library
  pylabhub_stage_libraries(TARGETS pylabhub::third_party::sodium)

  # Stage the header files
  pylabhub_stage_headers(
    DIRECTORIES "${install_dir}/include"
    SUBDIR "sodium"
  )
endif()
