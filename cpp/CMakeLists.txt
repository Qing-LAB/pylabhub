cmake_minimum_required(VERSION 3.15)
project(pylabhub-shell
        VERSION 0.1
        LANGUAGES CXX)

# ------------------------ Options ------------------------------------------
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (Linux/GNU Clang only)" OFF)
option(PREFER_VENDOR_NLOHMANN "Prefer vendored nlohmann/json under third_party/include if present" ON)

# Build pylabhub as shared library? (default: OFF -> build static library)
option(PYLABHUB_BUILD_SHARED "Build the pylabhub library as SHARED instead of STATIC" OFF)

# Install headers during "make install"?
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

# ------------------------ third_party / libzmq -----------------------------
set(ZMQ_LIB_TARGET "")  # will hold the target if libzmq is added
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

  # Use ZMQ_BUILD_TESTS so we don't override the top-level BUILD_TESTS
  if(NOT DEFINED ZMQ_BUILD_TESTS)
    set(ZMQ_BUILD_TESTS OFF CACHE BOOL "Disable libzmq tests" )
  endif()

  if(NOT DEFINED BUILD_PERF)
    set(BUILD_PERF OFF CACHE BOOL "Disable libzmq perf")
  endif()
  if(NOT DEFINED ENABLE_CURVE)
    set(ENABLE_CURVE OFF CACHE BOOL "Disable libzmq CURVE by default")
  endif()

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

if(ZMQ_LIB_TARGET)
  message(STATUS "Linking pylabhub against ${ZMQ_LIB_TARGET}")
  target_link_libraries(pylabhub PRIVATE ${ZMQ_LIB_TARGET})
endif()

# threads and c++ features
target_link_libraries(pylabhub PUBLIC Threads::Threads)
target_compile_features(pylabhub PUBLIC cxx_std_17)

# define LOGGER_EXPORTS while building the pylabhub target so logger.hpp exports correctly
target_compile_definitions(pylabhub PRIVATE LOGGER_EXPORTS)

# If building shared on Windows, export all symbols optionally
if(PYLABHUB_BUILD_SHARED AND MSVC)
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

# Install library and export targets (always)
install(TARGETS pylabhub
        EXPORT pylabhubTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Conditionally install headers (controlled by PYLABHUB_INSTALL_HEADERS)
if(PYLABHUB_INSTALL_HEADERS)
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
else()
  message(STATUS "PYLABHUB_INSTALL_HEADERS=OFF: skipping installation of headers under ${CMAKE_INSTALL_INCLUDEDIR}")
endif()

install(EXPORT pylabhubTargets FILE pylabhubTargets.cmake NAMESPACE pylabhub:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pylabhub)

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
message(STATUS "")
