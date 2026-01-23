# -----------------------------------------------------------------------------
# Language / Compiler flags / Build-type
# You must define project() before including this file.
# -----------------------------------------------------------------------------
# CMAKE_CXX_STANDARD is not set here to avoid forcing it on third-party libraries.
# It should be set on a per-target basis for project-specific code.
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# -------------------------------------------------------------------------------
# Platform detection & compile-time macros (robust, with diagnostics)
# -------------------------------------------------------------------------------
# We want exactly one PLATFORM_* macro defined and visible to all targets.
# Use add_compile_definitions() so macros are globally visible.

set(_platform_macro_defined FALSE)

if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "CMAKE_BUILD_TYPE not set. Default to Debug.")
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the build type: (Release, Debug, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

# Helpful diagnostics that will be printed so user can see what CMake sees.
message(STATUS "CMake generator information (diagnostics):")
message(STATUS "  CMAKE_GENERATOR...........: ${CMAKE_GENERATOR}")
message(STATUS "  CMAKE_GENERATOR_PLATFORM..: ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  CMAKE_VS_PLATFORM_NAME....: ${CMAKE_VS_PLATFORM_NAME}")
message(STATUS "  CMAKE_SIZEOF_VOID_P.......: ${CMAKE_SIZEOF_VOID_P}")
message(STATUS "  CMAKE_HOST_SYSTEM_PROCESSOR: ${CMAKE_HOST_SYSTEM_PROCESSOR}")
message(STATUS "  CMAKE_SYSTEM_NAME.........: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  CMAKE_BUILD_TYPE..........: ${CMAKE_BUILD_TYPE}")
message(STATUS "============================================================")

# Helper: robust predicate for "windows x64"
set(_is_windows FALSE)
if(WIN32)
  set(_is_windows TRUE)
endif()

# Determine if this configuration is x64 by checking several indicators.
# We treat it as x64 if any of the following is true:
#  - CMAKE_SIZEOF_VOID_P == 8 (most reliable),
#  - CMAKE_GENERATOR_PLATFORM mentions x64/Win64/AMD64,
#  - CMAKE_VS_PLATFORM_NAME equals x64,
#  - Host processor looks like x86_64/AMD64 (best-effort).
set(_is_x64 FALSE)
if(DEFINED CMAKE_SIZEOF_VOID_P AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_is_x64 TRUE)
else()
  # check platform strings (case-insensitive matches)
  string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _genplat_lc)
  string(TOLOWER "${CMAKE_VS_PLATFORM_NAME}" _vsplat_lc)
  string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _hostproc_lc)

  if(_genplat_lc MATCHES "x64|win64|amd64")
    set(_is_x64 TRUE)
  elseif(_vsplat_lc MATCHES "x64|win64|amd64")
    set(_is_x64 TRUE)
  elseif(_hostproc_lc MATCHES "x86_64|amd64|x64")
    # host processor is x86_64 — good hint, but not authoritative if cross-compiling.
    set(_is_x64 TRUE)
  endif()
endif()

# Platform classification and compile defs
if(_is_windows)
  set(PYLABHUB_IS_WINDOWS TRUE CACHE BOOL "Building for POSIX platform" FORCE)
  if(_is_x64)
    add_compile_definitions(PLATFORM_WIN64=1)
    set(PLATFORM_WIN64 TRUE CACHE BOOL "Building for Windows x64 platform" FORCE)
    set(_platform_macro_defined TRUE)
    set(_platform_name "Windows x64")
  else()
    # Enforce: project only supports 64-bit Windows
    add_compile_definitions(PLATFORM_WIN32=1)
    set(PLATFORM_WIN32 TRUE CACHE BOOL "Building for Windows x32 platform" FORCE)
    set(_platform_macro_defined TRUE)
    set(_platform_name "Windows x32")
    message(FATAL_ERROR "32-bit Windows toolchain detected. This project requires a 64-bit (x64) Windows generator/toolchain. Configure CMake with an x64 generator (e.g. -A x64) or select an x64 kit in your IDE.")
  endif()
elseif(APPLE)
  set(PYLABHUB_IS_POSIX TRUE CACHE BOOL "Building for POSIX platform" FORCE)
  add_compile_definitions(PLATFORM_APPLE=1)
  set(PLATFORM_APPLE TRUE CACHE BOOL "Building for Apple / macOS platform" FORCE)
  set(_platform_macro_defined TRUE)
  set(_platform_name "Apple / macOS")

  # Set a default deployment target if not specified by the user or is empty.
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version to target." FORCE)
  endif()
  message(STATUS "macOS deployment target set to: ${CMAKE_OSX_DEPLOYMENT_TARGET}")

  # Explicitly set the architecture based on the host processor.
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS architecture" FORCE)
  else()
    set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "macOS architecture" FORCE)
  endif()
  message(STATUS "macOS architecture set to: ${CMAKE_OSX_ARCHITECTURES}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(PYLABHUB_IS_POSIX TRUE CACHE BOOL "Building for POSIX platform" FORCE)
  add_compile_definitions(PLATFORM_FREEBSD=1)
  set(PLATFORM_FREEBSD TRUE CACHE BOOL "Building for FreeBSD platform" FORCE)
  set(_platform_macro_defined TRUE)
  set(_platform_name "FreeBSD")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "LINUX")
  set(PYLABHUB_IS_POSIX TRUE CACHE BOOL "Building for POSIX platform" FORCE)
  add_compile_definitions(PLATFORM_LINUX=1)
  set(PLATFORM_LINUX TRUE CACHE BOOL "Building for Linux platform" FORCE)
  set(_platform_macro_defined TRUE)
  set(_platform_name "Linux")
endif()

if(NOT _platform_macro_defined)
  # Fallback: unknown
  add_compile_definitions(PLATFORM_UNKNOWN=1)
  set(PLATFORM_UNKNOWN TRUE CACHE BOOL "Building for unknown platform" FORCE)
  set(_platform_name "Unknown")
endif()

# Informational summary
message(STATUS "")
message(STATUS "Platform compile-time macros summary:")
if(DEFINED PLATFORM_WIN64)
  message(STATUS "  Defined: PLATFORM_WIN64 (Windows x64)")
elseif(DEFINED PLATFORM_APPLE)
  message(STATUS "  Defined: PLATFORM_APPLE (macOS / Apple)")
elseif(DEFINED PLATFORM_FREEBSD)
  message(STATUS "  Defined: PLATFORM_FREEBSD (FreeBSD)")
elseif(DEFINED PLATFORM_LINUX)
  message(STATUS "  Defined: PLATFORM_LINUX (Linux)")
elseif(DEFINED PLATFORM_UNKNOWN)
  message(STATUS "  Defined: PLATFORM_UNKNOWN (Unknown platform)")
endif()
message(STATUS "============================================================")


# Default to Release if no type selected and generator is single-config.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, defaulting to Release")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
  message(STATUS "")
endif()

# Compiler warnings and parallel build options
if(MSVC)
  add_compile_options(/W4 /MP)
  message(STATUS "MSVC detected: enabling /W4 warnings and /MP parallel build")
  message(STATUS "")
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# -------------------------
# Force MSVC CRT globally
# -------------------------
# Vendor XOPSupport may use static CRT (LIBCMT). Force project to use static CRT (/MT)
if(MSVC)
  # Modern CMake: set global MSVC runtime library and force it into the cache.
  # MultiThreaded => /MT (Release), MultiThreadedDebug => /MTd (Debug)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "Select the MSVC runtime library." FORCE)
  message(STATUS "Forcing MSVC runtime library to ${CMAKE_MSVC_RUNTIME_LIBRARY}")
  message(STATUS "")
endif()

# ===========================================================================
# Clang-Tidy Static Analysis Integration
#
# This section handles the detection of clang-tidy and provides a helper
# function to apply it to our internal targets.
# ===========================================================================
if(PYLABHUB_ENABLE_CLANG_TIDY)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    find_program(PYLABHUB_CLANG_TIDY_EXE clang-tidy)
    if(PYLABHUB_CLANG_TIDY_EXE)
      message(STATUS "Clang-Tidy found: ${PYLABHUB_CLANG_TIDY_EXE}. Static analysis is active.")
      # This is required for clang-tidy to work correctly with the build system.
      set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    else()
      message(STATUS "Clang-Tidy not found. Skipping static analysis for this build.")
      # Disable the option for this run only, without changing the user's preference in the cache.
      set(PYLABHUB_ENABLE_CLANG_TIDY OFF)
    endif()
  else()
    message(WARNING "PYLABHUB_ENABLE_CLANG_TIDY is ON, but the current compiler is not Clang-based. Skipping static analysis.")
    set(PYLABHUB_ENABLE_CLANG_TIDY OFF)
  endif()
endif()

function(pylabhub_enable_clang_tidy_for_target TARGET_NAME)
  # The detection logic has already run. If PYLABHUB_ENABLE_CLANG_TIDY is still ON,
  # it means clang-tidy was found and is ready to use.
  if(PYLABHUB_ENABLE_CLANG_TIDY)
    if(TARGET ${TARGET_NAME})
      set_target_properties(${TARGET_NAME} PROPERTIES CXX_CLANG_TIDY "${PYLABHUB_CLANG_TIDY_EXE}")
    endif()
  endif()
endfunction()