cmake_minimum_required(VERSION 3.15)

# -----------------------------------------------------------------------------
# Platform detection & compile-time macros (single PLATFORM_* macro)
# -----------------------------------------------------------------------------
# The set: PLATFORM_WIN64, PLATFORM_APPLE, PLATFORM_FREEBSD, PLATFORM_LINUX, PLATFORM_UNKNOWN
set(_platform_macro_defined FALSE)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_compile_definitions(PLATFORM_WIN64=1)
  set(_platform_macro_defined TRUE)
  set(_platform_name "Windows x64")
elseif(APPLE)
  add_compile_definitions(PLATFORM_APPLE=1)
  set(_platform_macro_defined TRUE)
  set(_platform_name "Apple / macOS")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_compile_definitions(PLATFORM_FREEBSD=1)
  set(_platform_macro_defined TRUE)
  set(_platform_name "FreeBSD")
elseif(UNIX)
  add_compile_definitions(PLATFORM_LINUX=1)
  set(_platform_macro_defined TRUE)
  set(_platform_name "Unix-like (assumed Linux)")
endif()

if(NOT _platform_macro_defined)
  add_compile_definitions(PLATFORM_UNKNOWN=1)
  set(_platform_name "Unknown")
endif()

# Fail early for 32-bit Windows generator (project requires x64).
if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "32-bit Windows toolchain detected. This project only supports 64-bit Windows. Please configure a 64-bit (x64) generator/toolchain.")
endif()

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

# -----------------------------------------------------------------------------
# Language / Compiler flags / Build-type
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default to Release if no type selected and generator is single-config.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, defaulting to Release")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# Compiler warnings and parallel build options
if(MSVC)
  add_compile_options(/W4 /MP)
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
endif()

# -----------------------------------------------------------------------------
# Project declaration
# -----------------------------------------------------------------------------
project(pylabhub-shell
        VERSION 0.1
        LANGUAGES C CXX)

# -----------------------------------------------------------------------------
# CMake module path (for Find*.cmake files)
# -----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(cmake/FindXOPSupport.cmake)

# -----------------------------------------------------------------------------
# Top-level Options (keep project defaults, add third-party control knobs)
# -----------------------------------------------------------------------------
# Project-owned options (unchanged semantics)
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (Linux/GNU Clang only)" OFF)
option(PREFER_VENDOR_NLOHMANN "Prefer vendored nlohmann/json under third_party/include if present" ON)

option(PYLABHUB_BUILD_SHARED "Build the pylabhub library as SHARED instead of STATIC" OFF)
option(PYLABHUB_INSTALL_HEADERS "Install headers under ${CMAKE_INSTALL_INCLUDEDIR}" ON)

# ---------------------------------------------------------------------------
# Third-party policy knobs (these are wrapper *intent* variables)
# - These inform third_party/CMakeLists.txt which will map them to the exact
#   subproject options (fmt, libzmq, etc.). The wrapper will scope these
#   settings to subprojects so they do not leak to other libs or top-level.
# ---------------------------------------------------------------------------
# Force libzmq variant: "none" | "static" | "shared"
set(THIRD_PARTY_ZMQ_FORCE_VARIANT "static" CACHE STRING "Wrapper intent: force libzmq build variant" FORCE)

# Force fmt variant: "none" | "static" | "shared"
set(THIRD_PARTY_FMT_FORCE_VARIANT "static" CACHE STRING "Wrapper intent: force fmt build variant" FORCE)

# Wrapper-level intent to disable third-party tests by default (ON/OFF).
# The wrapper will set subproject-specific test knobs (e.g. ZMQ_BUILD_TESTS, FMT_TEST)
# rather than changing global BUILD_TESTS.
set(THIRD_PARTY_DISABLE_TESTS ON CACHE BOOL "Wrapper intent: disable third-party tests by default" FORCE)

# Wrapper-level intent on allowing upstream precompiled headers (OFF by default)
set(THIRD_PARTY_ALLOW_UPSTREAM_PCH OFF CACHE BOOL "Wrapper intent: allow upstream precompiled headers" FORCE)

message(STATUS "Top-level third-party policy: THIRD_PARTY_ZMQ_FORCE_VARIANT=${THIRD_PARTY_ZMQ_FORCE_VARIANT}, THIRD_PARTY_FMT_FORCE_VARIANT=${THIRD_PARTY_FMT_FORCE_VARIANT}, THIRD_PARTY_DISABLE_TESTS=${THIRD_PARTY_DISABLE_TESTS}")

# -----------------------------------------------------------------------------
# Find XOPSupport (vendored or system)
# -----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.15)
project(pylabhub-xop LANGUAGES C CXX)

# Allow override from command line:
option(USE_SYSTEM_XOP "Use system-installed XOP (headers/libs) instead of third_party vendor" OFF)
set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Local XOPSupport tree")

# expose include dir if vendor present
find_xopsupport(_xop_found)
if(_xop_found)
  message(STATUS "XOPSupport found; XOP::XOPSupport target is available.")
else()
  if(NOT USE_SYSTEM_XOP)
    message(WARNING "XOPSupport not found under ${XOP_VENDOR_DIR}. You can set -DUSE_SYSTEM_XOP=ON to use system headers/libs.")
  endif()
endif()

add_subdirectory(src/IgorXOP)

# -----------------------------------------------------------------------------
# Basic dependencies
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# nlohmann/json (vendored or FetchContent)
# -----------------------------------------------------------------------------
set(NLOHMANN_VENDORED OFF)
set(NLOHMANN_FETCHED OFF)

if(PREFER_VENDOR_NLOHMANN)
  set(NLOHMANN_HEADER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/include/nlohmann/json.hpp")
  if(EXISTS "${NLOHMANN_HEADER_PATH}")
    message(STATUS "Using vendored nlohmann/json at ${NLOHMANN_HEADER_PATH}")
    set(NLOHMANN_VENDORED ON)
  else()
    message(STATUS "Vendored nlohmann/json not found at ${NLOHMANN_HEADER_PATH}")
  endif()
endif()

if(NOT NLOHMANN_VENDORED)
  message(STATUS "Fetching nlohmann/json (header-only) via FetchContent")
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
  )
  FetchContent_MakeAvailable(nlohmann_json)
  set(NLOHMANN_FETCHED ON)
endif()

# -----------------------------------------------------------------------------
# Third-party submodules (fmt, libzmq, etc.)
# - The third_party/CMakeLists.txt is expected to implement scoped snapshot/restore
#   behavior so our THIRD_PARTY_* wrapper knobs control only the third-party subprojects.
# -----------------------------------------------------------------------------
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
if(EXISTS "${THIRD_PARTY_DIR}/CMakeLists.txt")
  message(STATUS "Adding ${THIRD_PARTY_DIR} to build (third_party/CMakeLists.txt)")
  add_subdirectory("${THIRD_PARTY_DIR}" EXCLUDE_FROM_ALL)
else()
  message(STATUS "third_party/CMakeLists.txt not found; you should add one to coordinate submodules (fmt, libzmq).")
endif()

# After third_party processing, attempt to detect canonical targets produced by subprojects
set(ZMQ_LIB_TARGET "")
if(TARGET zmq::zmq)
  set(ZMQ_LIB_TARGET "zmq::zmq")
elseif(TARGET zmq::libzmq)
  set(ZMQ_LIB_TARGET "zmq::libzmq")
elseif(TARGET libzmq)
  set(ZMQ_LIB_TARGET "libzmq")
elseif(TARGET zmq)
  set(ZMQ_LIB_TARGET "zmq")
else()
  message(STATUS "No libzmq target detected in CMake registry after third_party processing.")
endif()

set(FMT_TARGET "")
if(TARGET fmt::fmt)
  set(FMT_TARGET "fmt::fmt")
elseif(TARGET fmt::fmt-header-only)
  set(FMT_TARGET "fmt::fmt-header-only")
else()
  message(STATUS "fmt CMake target not detected yet. Ensure third_party adds fmt::fmt or fmt::fmt-header-only, or install fmt system-wide.")
endif()

# -----------------------------------------------------------------------------
# Source discovery
# -----------------------------------------------------------------------------
file(GLOB_RECURSE UTIL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp")
file(GLOB_RECURSE FILEUTIL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/fileutil/*.cpp")

if(NOT UTIL_SOURCES)
  message(WARNING "No sources found in src/util — check your source path.")
endif()
if(NOT FILEUTIL_SOURCES)
  message(WARNING "No sources found in src/fileutil — check your source path.")
endif()

# -----------------------------------------------------------------------------
# Build: create the single combined library target (pylabhub-corelib)
# -----------------------------------------------------------------------------
set(PYLABHUB_CORELIB_SOURCES ${UTIL_SOURCES} ${FILEUTIL_SOURCES})

if(PYLABHUB_BUILD_SHARED)
  add_library(pylabhub-corelib SHARED ${PYLABHUB_CORELIB_SOURCES})
else()
  add_library(pylabhub-corelib STATIC ${PYLABHUB_CORELIB_SOURCES})
endif()

add_library(pylabhub::pylabhub-corelib ALIAS pylabhub-corelib)

target_include_directories(pylabhub-corelib
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
)

# Add vendored nlohmann include path if present
if(NLOHMANN_VENDORED)
  target_include_directories(pylabhub-corelib
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/include>
      $<INSTALL_INTERFACE:include>
  )
endif()

# -----------------------------------------------------------------------------
# Link third-party targets (if available)
# -----------------------------------------------------------------------------
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(pylabhub-corelib PUBLIC nlohmann_json::nlohmann_json)
endif()

if(FMT_TARGET)
  message(STATUS "Linking pylabhub-corelib against ${FMT_TARGET}")
  target_link_libraries(pylabhub-corelib PRIVATE ${FMT_TARGET})
endif()

if(ZMQ_LIB_TARGET)
  message(STATUS "Linking pylabhub-corelib against ${ZMQ_LIB_TARGET}")
  target_link_libraries(pylabhub-corelib PRIVATE ${ZMQ_LIB_TARGET})
else()
  # Try system-wide ZeroMQ as fallback
  find_package(ZeroMQ QUIET)
  if(ZeroMQ_FOUND)
    message(STATUS "ZeroMQ found by find_package; linking against system ZeroMQ")
    if(TARGET ZeroMQ::ZeroMQ)
      target_link_libraries(pylabhub-corelib PRIVATE ZeroMQ::ZeroMQ)
    elseif(TARGET ZeroMQ::zmq)
      target_link_libraries(pylabhub-corelib PRIVATE ZeroMQ::zmq)
    else()
      if(DEFINED ZeroMQ_LIBRARIES)
        target_link_libraries(pylabhub-corelib PRIVATE ${ZeroMQ_LIBRARIES})
      endif()
      if(DEFINED ZeroMQ_INCLUDE_DIRS)
        target_include_directories(pylabhub-corelib PRIVATE ${ZeroMQ_INCLUDE_DIRS})
      endif()
    endif()
  else()
    message(STATUS "No ZeroMQ target found; libzmq may not be available. If you need ZeroMQ, add it under third_party or install system libzmq.")
  endif()
endif()

# Link threads and enforce C++ language features
target_link_libraries(pylabhub-corelib PUBLIC Threads::Threads)
target_compile_features(pylabhub-corelib PUBLIC cxx_std_17)

# Windows-specific: when building pylabhub-corelib as shared on MSVC x64, optionally export symbols
if(PYLABHUB_BUILD_SHARED AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set_target_properties(pylabhub-corelib PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# -----------------------------------------------------------------------------
# Sanitizers (only enabled on supported compilers/platforms)
# -----------------------------------------------------------------------------
if(ENABLE_SANITIZERS)
    if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
        message(WARNING "ENABLE_SANITIZERS requested but compiler (${CMAKE_CXX_COMPILER_ID}) is not supported. Disabling sanitizers.")
        set(ENABLE_SANITIZERS OFF CACHE BOOL "Enable sanitizers" FORCE)
    endif()
    if(APPLE)
        message(WARNING "Sanitizers requested but running on macOS. Disabling sanitizers by default on Apple platforms.")
        set(ENABLE_SANITIZERS OFF CACHE BOOL "Enable sanitizers" FORCE)
    endif()
endif()

if(ENABLE_SANITIZERS)
    message(STATUS "Enabling ASAN/UBSAN sanitizers for pylabhub (compiler: ${CMAKE_CXX_COMPILER_ID})")
    target_compile_options(pylabhub-corelib PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pylabhub-corelib PUBLIC -fsanitize=address,undefined)
endif()

set_target_properties(pylabhub-corelib PROPERTIES POSITION_INDEPENDENT_CODE ON)



# -----------------------------------------------------------------------------
# Build: create the main executable target (pylabhub-shell)
# -----------------------------------------------------------------------------

# main executable that uses the library
add_executable(pylabhub-shell
  src/coreshell.cpp
)

target_link_libraries(pylabhub-shell
  PRIVATE pylabhub-corelib
)


# -----------------------------------------------------------------------------
# Install / export (kept commented as before; left for your preference)
# -----------------------------------------------------------------------------
include(GNUInstallDirs)

# (installation blocks commented out per original file — unchanged)
# install(TARGETS pylabhub ...)

# -----------------------------------------------------------------------------
# Tests for this project (your project-owned tests)
# - IMPORTANT: this is controlled by BUILD_TESTS (top-level). We DID NOT change
#   or override BUILD_TESTS in the third-party wrapper. Third-party tests are
#   controlled independently by THIRD_PARTY_DISABLE_TESTS and mapped to
#   subproject-specific knobs inside third_party/CMakeLists.txt.
# -----------------------------------------------------------------------------
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Final summary (concise)
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "CMake summary:")
message(STATUS "  Project:            ${PROJECT_NAME} (${PROJECT_VERSION})")
message(STATUS "  CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER}")
message(STATUS "  Build type:         ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build tests:        ${BUILD_TESTS}   (project-owned tests)")
message(STATUS "  Third-party tests disabled (wrapper intent): ${THIRD_PARTY_DISABLE_TESTS}")
message(STATUS "  Sanitizers:         ${ENABLE_SANITIZERS}")
message(STATUS "  Using vendored nlohmann: ${NLOHMANN_VENDORED}")
message(STATUS "  libzmq target: ${ZMQ_LIB_TARGET}")
message(STATUS "  fmt target: ${FMT_TARGET}")
message(STATUS "  pylabhub build shared: ${PYLABHUB_BUILD_SHARED}")
message(STATUS "  Install headers: ${PYLABHUB_INSTALL_HEADERS}")
if(DEFINED _platform_name)
  message(STATUS "  Platform detected:  ${_platform_name}")
endif()
message(STATUS "")
