cmake_minimum_required(VERSION 3.15)
project(pylabhub-shell
        VERSION 0.1
        LANGUAGES CXX)

# ---------------------------------------------------------
# Options
# ---------------------------------------------------------
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (Linux/macOS only)" OFF)
# Prefer vendored nlohmann/json inside third_party/include. If not present, FetchContent will fetch it.
option(PREFER_VENDOR_NLOHMANN "Prefer vendored nlohmann/json under third_party/include if present" ON)
# Use CMake-wide BUILD_SHARED_LIBS to control shared vs static. Users can override.
# (You can still provide a custom option if you prefer.)
# option(BUILD_SHARED_LIBS "Build shared libraries instead of static where applicable" OFF)

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
    GIT_TAG v3.12.0
  )
  FetchContent_MakeAvailable(nlohmann_json)
  set(NLOHMANN_FETCHED ON)
endif()

# ---------------------------------------------------------
# Build library target pylabhub-util
# Sources live under src/util/ — update the path if your layout differs
# ---------------------------------------------------------
file(GLOB_RECURSE UTIL_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp"
)

if(NOT UTIL_SOURCES)
  message(WARNING "No sources found in ${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp — check your source path.")
endif()

# Automatically add submodules that expose a CMakeLists.txt
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

if(EXISTS "${THIRD_PARTY_DIR}")
  file(GLOB SUBMODULE_DIRS LIST_DIRECTORIES true "${THIRD_PARTY_DIR}/*")
  foreach(_subdir IN LISTS SUBMODULE_DIRS)
    if(IS_DIRECTORY "${_subdir}" AND EXISTS "${_subdir}/CMakeLists.txt")
      get_filename_component(_name ${_subdir} NAME)
      message(STATUS "Adding third_party subdir: ${_name}")
      add_subdirectory("${_subdir}" "${CMAKE_CURRENT_BINARY_DIR}/third_party/${_name}")
    endif()
  endforeach()
endif()

# Create library; allow BUILD_SHARED_LIBS to control shared/static
add_library(pylabhub-util ${UTIL_SOURCES})

# Provide an alias target for consumers
add_library(pylabhub::util ALIAS pylabhub-util)

target_include_directories(pylabhub-util
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
)

# If vendored header present, add third_party include dir
if(NLOHMANN_VENDORED)
    target_include_directories(pylabhub-util PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/include
    )
endif()

# If FetchContent provided a target, link it to expose interface (header-only)
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(pylabhub-util PUBLIC nlohmann_json::nlohmann_json)
endif()

# Link threads
target_link_libraries(pylabhub-util PUBLIC Threads::Threads)

# Express C++ standard requirement for consumers
target_compile_features(pylabhub-util PUBLIC cxx_std_17)

# Optional sanitizer flags for GCC/Clang
if(ENABLE_SANITIZERS AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
    message(STATUS "Enabling ASAN/UBSAN sanitizers for pylabhub-util")
    target_compile_options(pylabhub-util PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pylabhub-util PUBLIC -fsanitize=address,undefined)
endif()

# Installation rules (optional)
include(GNUInstallDirs)
install(TARGETS pylabhub-util EXPORT pylabhub-utilTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Export the target for downstream use if you plan to 'install' this project
install(EXPORT pylabhub-utilTargets FILE pylabhub-utilTargets.cmake NAMESPACE pylabhub:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pylabhub-util)

# ---------------------------------------------------------
# Tests (delegated to tests/CMakeLists.txt)
# ---------------------------------------------------------
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Summary
message(STATUS "")
message(STATUS "CMake summary:")
message(STATUS "  Project:            ${PROJECT_NAME} (${PROJECT_VERSION})")
message(STATUS "  CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER}")
message(STATUS "  Build type:         ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build tests:        ${BUILD_TESTS}")
message(STATUS "  Sanitizers:         ${ENABLE_SANITIZERS}")
message(STATUS "  Using vendored nlohmann: ${NLOHMANN_VENDORED}")
message(STATUS "  Fetched nlohmann: ${NLOHMANN_FETCHED}")
message(STATUS "")


