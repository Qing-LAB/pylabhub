# -----------------------------------------------------------------------------
# Language / Compiler flags / Build-type
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# -------------------------------------------------------------------------------
# Platform detection & compile-time macros (robust, with diagnostics)
# -------------------------------------------------------------------------------
# We want exactly one PLATFORM_* macro defined and visible to all targets.
# Use add_compile_definitions() so macros are globally visible.

set(_platform_macro_defined FALSE)

# Helpful diagnostics that will be printed so user can see what CMake sees.
message(STATUS "CMake generator information (diagnostics):")
message(STATUS "  CMAKE_GENERATOR...........: ${CMAKE_GENERATOR}")
message(STATUS "  CMAKE_GENERATOR_PLATFORM..: ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  CMAKE_VS_PLATFORM_NAME....: ${CMAKE_VS_PLATFORM_NAME}")
message(STATUS "  CMAKE_SIZEOF_VOID_P.......: ${CMAKE_SIZEOF_VOID_P}")
message(STATUS "  CMAKE_HOST_SYSTEM_PROCESSOR: ${CMAKE_HOST_SYSTEM_PROCESSOR}")
message(STATUS "  CMAKE_SYSTEM_NAME.........: ${CMAKE_SYSTEM_NAME}")
message(STATUS "")

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
  add_compile_definitions(PLATFORM_APPLE=1)
  set(PLATFORM_APPLE TRUE CACHE BOOL "Building for Apple / macOS platform" FORCE)
  set(_platform_macro_defined TRUE)
  set(_platform_name "Apple / macOS")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_compile_definitions(PLATFORM_FREEBSD=1)
  set(PLATFORM_FREEBSD TRUE CACHE BOOL "Building for FreeBSD platform" FORCE)
  set(_platform_macro_defined TRUE)
  set(_platform_name "FreeBSD")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "LINUX")
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
message(STATUS "")


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
# Vendor XOPSupport uses static CRT (LIBCMT). Force project to use static CRT (/MT)
if(MSVC)
  # Modern CMake: set global MSVC runtime library and force it into the cache.
  # MultiThreaded => /MT (Release), MultiThreadedDebug => /MTd (Debug)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "CRT library" FORCE)

  # As a redundancy for older CMake versions or generators that respect CMAKE_CXX_FLAGS_*
  # set the initial flags so targets created later will pick them up.
  if(NOT DEFINED CMAKE_C_FLAGS_INIT)
    set(CMAKE_C_FLAGS_INIT "/MT" CACHE STRING "" FORCE)
  endif()
  if(NOT DEFINED CMAKE_CXX_FLAGS_INIT)
    set(CMAKE_CXX_FLAGS_INIT "/MT" CACHE STRING "" FORCE)
  endif()

  message(STATUS "Forcing MSVC runtime library to ${CMAKE_MSVC_RUNTIME_LIBRARY}")
  message(STATUS "")
endif()
