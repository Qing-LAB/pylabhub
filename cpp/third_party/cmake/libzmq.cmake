# third_party/cmake/libzmq.cmake
#
# This script defines the ExternalProject_Add target for building libzmq.
# It follows the project's "Prerequisite Build System" pattern, where the library
# is built externally and installed into a sandboxed `prereqs` directory.
#
# The integration of this pre-built artifact (e.g., creating an IMPORTED
# target and registering it for staging) is handled by the parent
# `third_party/CMakeLists.txt` file, mirroring the pattern used by libsodium.
#
include(ExternalProject)
include(ThirdPartyPolicyAndHelper)

if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

# --- 1. libzmq External Project Definition ---
set(LIBZMQ_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libzmq")
set(LIBZMQ_BUILD_DIR "${CMAKE_BINARY_DIR}/third_party/libzmq-build")

if(MSVC)
  set(_sodium_lib "${PREREQ_INSTALL_DIR}/lib/libsodium.lib")
else()
  set(_sodium_lib "${PREREQ_INSTALL_DIR}/lib/libsodium.a")
endif()

# These arguments are passed to the sub-project's CMake configuration step.
set(_zmq_cmake_args
  "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
  "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
  "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
  "-DBUILD_STATIC=ON"
  # WORKAROUND: Build shared library as well to handle libzmq internal bug.
  # The libzmq build script for its 'curve_keygen' tool incorrectly links
  # against the shared library target ('libzmq') even when only a static build
  # is requested. Building both satisfies this internal dependency. Our parent
  # project is configured to only use and stage the static library artifact,
  # so the shared library is effectively a transient build artifact.
  "-DBUILD_SHARED=ON"
  "-DZMQ_BUILD_TESTS=OFF"
  "-DWITH_PERF_TOOL=OFF"
  "-DWITH_DOCS=OFF"
  "-DENABLE_CURVE=ON"
  "-DWITH_LIBSODIUM=ON"
  "-DSODIUM_INCLUDE_DIRS:PATH=${PREREQ_INSTALL_DIR}/include"
  "-DSODIUM_LIBRARIES:FILEPATH=${_sodium_lib}"
)

# Define a robust custom install command that copies build outputs to the
# PREREQ_INSTALL_DIR. This is necessary for two reasons:
# 1. It gives us full control over the header directory structure, allowing us
#    to create the desired 'zmq/' subdirectory for hierarchical includes.
# 2. It robustly copies the entire library output directory, avoiding the need
#    to guess platform-specific library filenames.
set(_zmq_custom_install
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PREREQ_INSTALL_DIR}/include/zmq"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${LIBZMQ_SOURCE_DIR}/include/" "${PREREQ_INSTALL_DIR}/include/zmq/"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${LIBZMQ_BUILD_DIR}/lib/" "${PREREQ_INSTALL_DIR}/lib/"
)

ExternalProject_Add(libzmq_external
  SOURCE_DIR      ${LIBZMQ_SOURCE_DIR}
  BINARY_DIR      ${LIBZMQ_BUILD_DIR}
  INSTALL_DIR     ${PREREQ_INSTALL_DIR}
  CMAKE_ARGS      ${_zmq_cmake_args}

  INSTALL_COMMAND ${_zmq_custom_install}
  BUILD_BYPRODUCTS "${PREREQ_INSTALL_DIR}/include/zmq/zmq.h"
  DEPENDS libsodium_external
)

# Add the external project to the global prerequisite hook target.
add_dependencies(build_prerequisites libzmq_external)

message(STATUS "[pylabhub-third-party] Defined ExternalProject 'libzmq_external'")
