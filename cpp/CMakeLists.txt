cmake_minimum_required(VERSION 3.15)
project(pylabhub-shell
        VERSION 0.1
        LANGUAGES CXX)

# ------------------------ Options ------------------------------------------
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (Linux/GNU Clang only)" OFF)
option(PREFER_VENDOR_NLOHMANN "Prefer vendored nlohmann/json under third_party/include if present" ON)

option(PYLABHUB_BUILD_SHARED "Build the pylabhub library as SHARED instead of STATIC" OFF)
option(PYLABHUB_INSTALL_HEADERS "Install headers under ${CMAKE_INSTALL_INCLUDEDIR}" ON)

# ------------------------ Language / flags --------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, defaulting to Release")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /MP)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ------------------------ Platform detection & compile-time macros ----------
# We want exactly one PLATFORM_* macro defined and visible to all targets. Use add_compile_definitions()
# so the macros are globally visible (also will be applied to targets created later).
#
# The macro set is:
#   PLATFORM_WIN64, PLATFORM_APPLE, PLATFORM_FREEBSD, PLATFORM_LINUX, PLATFORM_UNKNOWN
#
# For Windows we require 64-bit; if a 32-bit Windows generator/toolchain is selected we fail early.

# Start by clearing any of these (safe-guard)
# (CMake doesn't provide a direct "undefine" for compile definitions, but we control what we add below)

set(_platform_macro_defined FALSE)

# Windows 64-bit (CMake variable WIN32 is true for any Windows; check pointer size to ensure x64)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_compile_definitions(PLATFORM_WIN64=1)
  set(_platform_macro_defined TRUE)
  set(_platform_name "Windows x64")
elseif(APPLE) # Apple / macOS
  add_compile_definitions(PLATFORM_APPLE=1)
  set(_platform_macro_defined TRUE)
  set(_platform_name "Apple / macOS")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_compile_definitions(PLATFORM_FREEBSD=1)
  set(_platform_macro_defined TRUE)
  set(_platform_name "FreeBSD")
elseif(UNIX) # Non-Apple UNIX-like -> treat as Linux by default
  add_compile_definitions(PLATFORM_LINUX=1)
  set(_platform_macro_defined TRUE)
  set(_platform_name "Unix-like (assumed Linux)")
endif()

if(NOT _platform_macro_defined)
  # fallback
  add_compile_definitions(PLATFORM_UNKNOWN=1)
  set(_platform_name "Unknown")
endif()

# Enforce that we don't try to configure 32-bit Windows - fail early rather than produce subtle bugs
if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "32-bit Windows toolchain detected. This project only supports 64-bit Windows. Please configure a 64-bit (x64) generator/toolchain.")
endif()

# Provide an informational summary
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

# ------------------------ Basic deps --------------------------------------
find_package(Threads REQUIRED)

# ------------------------ nlohmann/json -----------------------------------
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

# ------------------------ third_party -----------------------------
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
# If the user added a dedicated third_party/CMakeLists.txt (you said you have), let it take care of submodules (fmt, libzmq, etc.)
if(EXISTS "${THIRD_PARTY_DIR}/CMakeLists.txt")
    message(STATUS "Adding ${THIRD_PARTY_DIR} to build (third_party/CMakeLists.txt)")
    add_subdirectory("${THIRD_PARTY_DIR}" EXCLUDE_FROM_ALL)
else()
    message(STATUS "third_party/CMakeLists.txt not found; you should add one to coordinate submodules (fmt, libzmq).")
endif()

# After third_party processing, try to detect common targets (fmt, libzmq/zmq)
set(ZMQ_LIB_TARGET "")  # will hold the target if libzmq is available
if(TARGET zmq::zmq)
  set(ZMQ_LIB_TARGET "zmq::zmq")
elseif(TARGET zmq::libzmq)
  set(ZMQ_LIB_TARGET "zmq::libzmq")
elseif(TARGET libzmq)
  set(ZMQ_LIB_TARGET "libzmq")
elseif(TARGET zmq)
  set(ZMQ_LIB_TARGET "zmq")
else()
  # not fatal: maybe libzmq is provided by system find_package later
  message(STATUS "No libzmq target detected in CMake registry after third_party processing.")
endif()

# Detect fmt targets if present (third_party/fmt or system)
set(FMT_TARGET "")
if(TARGET fmt::fmt)
  set(FMT_TARGET "fmt::fmt")
elseif(TARGET fmt::fmt-header-only)
  set(FMT_TARGET "fmt::fmt-header-only")
else()
  message(STATUS "fmt CMake target not detected yet. Ensure third_party adds fmt::fmt or fmt::fmt-header-only, or install fmt system-wide.")
endif()

# ------------------------ source discovery --------------------------------
file(GLOB_RECURSE UTIL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp")
file(GLOB_RECURSE FILEUTIL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/fileutil/*.cpp")

if(NOT UTIL_SOURCES)
  message(WARNING "No sources found in src/util — check your source path.")
endif()
if(NOT FILEUTIL_SOURCES)
  message(WARNING "No sources found in src/fileutil — check your source path.")
endif()

# ------------------------ Single combined library -------------------------
set(PYLABHUB_SOURCES ${UTIL_SOURCES} ${FILEUTIL_SOURCES})

if(PYLABHUB_BUILD_SHARED)
  add_library(pylabhub SHARED ${PYLABHUB_SOURCES})
else()
  add_library(pylabhub STATIC ${PYLABHUB_SOURCES})
endif()

add_library(pylabhub::pylabhub ALIAS pylabhub)

target_include_directories(pylabhub
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
)

# Add vendored nlohmann include path if present
if(NLOHMANN_VENDORED)
  target_include_directories(pylabhub
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/include>
      $<INSTALL_INTERFACE:include>
  )
endif()

# Link third-party targets if available
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(pylabhub PUBLIC nlohmann_json::nlohmann_json)
endif()

# Link fmt if detected via third_party or system.
if(FMT_TARGET)
  message(STATUS "Linking pylabhub against ${FMT_TARGET}")
  # link PUBLIC so downstream consumers inherit fmt include and link usage
  target_link_libraries(pylabhub PRIVATE ${FMT_TARGET})
endif()

# Link libzmq if third_party produced a target
if(ZMQ_LIB_TARGET)
  message(STATUS "Linking pylabhub against ${ZMQ_LIB_TARGET}")
  target_link_libraries(pylabhub PRIVATE ${ZMQ_LIB_TARGET})
else()
  # fallback: try find_package(ZeroMQ) to get system-provided zmq if needed
  find_package(ZeroMQ QUIET)
  if(ZeroMQ_FOUND)
    message(STATUS "ZeroMQ found by find_package; linking against system ZeroMQ")
    if(TARGET ZeroMQ::ZeroMQ)
      target_link_libraries(pylabhub PRIVATE ZeroMQ::ZeroMQ)
    elseif(TARGET ZeroMQ::zmq)
      target_link_libraries(pylabhub PRIVATE ZeroMQ::zmq)
    else()
      if(DEFINED ZeroMQ_LIBRARIES)
        target_link_libraries(pylabhub PRIVATE ${ZeroMQ_LIBRARIES})
      endif()
      if(DEFINED ZeroMQ_INCLUDE_DIRS)
        target_include_directories(pylabhub PRIVATE ${ZeroMQ_INCLUDE_DIRS})
      endif()
    endif()
  else()
    message(STATUS "No ZeroMQ target found; libzmq may not be available. If you need ZeroMQ, either add it under third_party and ensure third_party/CMakeLists.txt adds it, or install system libzmq.")
  endif()
endif()

# threads and c++ features
target_link_libraries(pylabhub PUBLIC Threads::Threads)
target_compile_features(pylabhub PUBLIC cxx_std_17)

# define LOGGER_EXPORTS while building the pylabhub target so logger.hpp exports correctly
target_compile_definitions(pylabhub PRIVATE LOGGER_EXPORTS)

# If building shared on Windows (and on a 64-bit toolchain), export all symbols optionally
if(PYLABHUB_BUILD_SHARED AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set_target_properties(pylabhub PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# sanitizers: enable only on supported compilers/platforms
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
    target_compile_options(pylabhub PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pylabhub PUBLIC -fsanitize=address,undefined)
endif()

set_target_properties(pylabhub PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ------------------------ install & export --------------------------------
include(GNUInstallDirs)

# install(TARGETS pylabhub
#         EXPORT pylabhubTargets
#         ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
#         LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
#         RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
#         PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
# )

# if(PYLABHUB_INSTALL_HEADERS)
#   install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# else()
#   message(STATUS "PYLABHUB_INSTALL_HEADERS=OFF: skipping installation of headers under ${CMAKE_INSTALL_INCLUDEDIR}")
# endif()

# install(EXPORT pylabhubTargets FILE pylabhubTargets.cmake NAMESPACE pylabhub:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pylabhub)

# ------------------------ Tests -------------------------------------------
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ------------------------ Summary -----------------------------------------
message(STATUS "")
message(STATUS "CMake summary:")
message(STATUS "  Project:            ${PROJECT_NAME} (${PROJECT_VERSION})")
message(STATUS "  CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER}")
message(STATUS "  Build type:         ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build tests:        ${BUILD_TESTS}")
message(STATUS "  Sanitizers:         ${ENABLE_SANITIZERS}")
message(STATUS "  Using vendored nlohmann: ${NLOHMANN_VENDORED}")
message(STATUS "  libzmq target: ${ZMQ_LIB_TARGET}")
message(STATUS "  pylabhub build shared: ${PYLABHUB_BUILD_SHARED}")
message(STATUS "  Install headers: ${PYLABHUB_INSTALL_HEADERS}")
if(DEFINED _platform_name)
  message(STATUS "  Platform detected:  ${_platform_name}")
endif()
message(STATUS "")
