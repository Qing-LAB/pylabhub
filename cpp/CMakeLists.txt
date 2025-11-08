cmake_minimum_required(VERSION 3.15)

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

# -----------------------------------------------------------------------------
# Top level options and policies
# -----------------------------------------------------------------------------
include(cmake/PlatformAndCompiler.cmake)
include(cmake/ToplevelOptions.cmake)
include(cmake/ThirdPartyPolicy.cmake)

# -----------------------------------------------------------------------------
# Helper function to find XOPSupport
# -----------------------------------------------------------------------------
include(cmake/FindXOPSupport.cmake)

# -----------------------------------------------------------------------------
# Find XOPSupport (vendored or system)
# -----------------------------------------------------------------------------
message(STATUS "==============================================")
message(STATUS "Setting up BUILD_XOP and locating XOPSupport...")
if(NOT (DEFINED PLATFORM_WIN64 OR DEFINED PLATFORM_APPLE))
  set(BUILD_XOP OFF CACHE BOOL "Build the pylabhub XOP plugin (only supported on macOS and Windows x64)" FORCE)
  message(STATUS "XOP only supported on Windows x64 or macOS; disabling BUILD_XOP.")
endif()

if(BUILD_XOP)
# Ask the find helper to locate XOPSupport (uses XOP_VENDOR_DIR and USE_SYSTEM_XOP)
  message(STATUS "BUILD_XOP=ON; attempting to locate XOPSupport...")
  find_xopsupport(_xop_found)
  if(NOT _xop_found)
    message(WARNING "XOPSupport not found; BUILD_XOP will be ignored.")
    set(BUILD_XOP OFF CACHE BOOL "Build the pylabhub XOP plugin (only supported on macOS and Windows x64)" FORCE)
  else()
    message(STATUS "XOPSupport available; XOP build enabled.")
    # XOP::XOPSupport target is now available for linking
  endif()
endif()

# Only add the IgorXOP subdirectory if we intend to build XOP and XOP support exists.
if(BUILD_XOP AND _xop_found)
  add_subdirectory(src/IgorXOP)
else()
  if(NOT BUILD_XOP)
    message(STATUS "IgorXOP: BUILD_XOP=OFF; IgorXOP subproject skipped.")
  else()
    message(STATUS "IgorXOP: IgorXOP subproject skipped because XOPSupport not available.")
  endif()
endif()
message(STATUS "==============================================")

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
message(STATUS "==============================================")
message(STATUS "CMake summary:")
if(DEFINED _platform_name)
  message(STATUS "  Platform detected:  ${_platform_name}")
endif()
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
message(STATUS "==============================================")

if(_build_xop_final)
  message(STATUS "  XOP build added:    ${_build_xop_final}")
  message(STATUS "  XOP vendor present: ${_xop_vendor_present}")
  message(STATUS "==============================================")
else()
  message(STATUS "  XOP build:    OFF (IgorXOP subproject skipped)")
  message(STATUS "==============================================")
endif()


