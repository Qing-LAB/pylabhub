cmake_minimum_required(VERSION 3.15)
project(pylabhub-shell
        VERSION 0.1
        LANGUAGES CXX)

# ---------------------------------------------------------
# Options
# ---------------------------------------------------------
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (Linux/macOS only)" OFF)
option(PREFER_VENDOR_NLOHMANN "Prefer vendored nlohmann/json under third_party/include if present" ON)

# ---------------------------------------------------------
# Compiler / flags
# ---------------------------------------------------------
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

# ---------------------------------------------------------
# Basic deps
# ---------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------
# nlohmann/json: prefer vendored header under third_party/include/nlohmann/json.hpp
# If not present, fetch via FetchContent and use the provided target.
# ---------------------------------------------------------
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

# ---------------------------------------------------------
# Third-party modules
# (unchanged) - libzmq integration retained if present
# ---------------------------------------------------------
set(ZMQ_LIB_TARGET "")  # will hold the target name if libzmq is added
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(THIRD_PARTY_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/third_party" CACHE PATH "Where to build third-party subprojects")

if(EXISTS "${THIRD_PARTY_DIR}/libzmq/CMakeLists.txt")
  message(STATUS "Found libzmq submodule: adding to build")

  option(LIBZMQ_BUILD_SHARED "Build libzmq as shared library (default OFF)" OFF)
  if(LIBZMQ_BUILD_SHARED)
    set(LIBZMQ_SHARED_OPT ON CACHE BOOL "Build libzmq shared" FORCE)
  else()
    set(LIBZMQ_SHARED_OPT OFF CACHE BOOL "Build libzmq static" FORCE)
  endif()

  set(BUILD_TESTS OFF CACHE BOOL "Disable libzmq tests" FORCE)
  set(BUILD_PERF OFF CACHE BOOL "Disable libzmq perf" FORCE)
  set(ENABLE_CURVE OFF CACHE BOOL "Disable libzmq CURVE by default" FORCE)

  set(ZMQ_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/third_party/libzmq_build-${CMAKE_SYSTEM_NAME}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_BUILD_TYPE}")
  file(MAKE_DIRECTORY "${ZMQ_BUILD_DIR}")
  add_subdirectory("${THIRD_PARTY_DIR}/libzmq" "${ZMQ_BUILD_DIR}")

  if(TARGET zmq::libzmq)
    set(ZMQ_LIB_TARGET "zmq::libzmq")
  elseif(TARGET libzmq)
    set(ZMQ_LIB_TARGET "libzmq")
  elseif(TARGET zmq)
    set(ZMQ_LIB_TARGET "zmq")
  else()
    message(WARNING "libzmq added but no known CMake target found (expected zmq::libzmq or libzmq).")
  endif()
else()
  message(STATUS "libzmq submodule not present under ${THIRD_PARTY_DIR}/libzmq - skipping libzmq build")
endif()

# ---------------------------------------------------------
# Build library target pylabhub-util
# ---------------------------------------------------------
# include both src/util and src/fileutil to pick up the current sources layout
file(GLOB_RECURSE UTIL_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/fileutil/*.cpp"
)

if(NOT UTIL_SOURCES)
  message(WARNING "No sources found in src/util or src/fileutil — check your source path.")
endif()

add_library(pylabhub-util ${UTIL_SOURCES})
add_library(pylabhub::util ALIAS pylabhub-util)

target_include_directories(pylabhub-util
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
)

if(NLOHMANN_VENDORED)
  target_include_directories(pylabhub-util
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/include>
      $<INSTALL_INTERFACE:include>
  )
endif()

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(pylabhub-util PUBLIC nlohmann_json::nlohmann_json)
endif()

if(ZMQ_LIB_TARGET)
  message(STATUS "Linking pylabhub-util against ${ZMQ_LIB_TARGET}")
  target_link_libraries(pylabhub-util PRIVATE ${ZMQ_LIB_TARGET})
endif()

target_link_libraries(pylabhub-util PUBLIC Threads::Threads)
target_compile_features(pylabhub-util PUBLIC cxx_std_17)

if(ENABLE_SANITIZERS AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
    message(STATUS "Enabling ASAN/UBSAN sanitizers for pylabhub-util")
    target_compile_options(pylabhub-util PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pylabhub-util PUBLIC -fsanitize=address,undefined)
endif()

include(GNUInstallDirs)
install(TARGETS pylabhub-util EXPORT pylabhub-utilTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT pylabhub-utilTargets FILE pylabhub-utilTargets.cmake NAMESPACE pylabhub:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pylabhub-util)

# ---------------------------------------------------------
# Tests
# ---------------------------------------------------------
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------
# Summary
# ---------------------------------------------------------
message(STATUS "")
message(STATUS "CMake summary:")
message(STATUS "  Project:            ${PROJECT_NAME} (${PROJECT_VERSION})")
message(STATUS "  CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER}")
message(STATUS "  Build type:         ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build tests:        ${BUILD_TESTS}")
message(STATUS "  Sanitizers:         ${ENABLE_SANITIZERS}")
message(STATUS "  Using vendored nlohmann: ${NLOHMANN_VENDORED}")
message(STATUS "  libzmq target: ${ZMQ_LIB_TARGET}")
message(STATUS "")
