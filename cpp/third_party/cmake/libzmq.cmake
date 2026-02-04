# third_party/cmake/libzmq.cmake
#
# This script is a wrapper for building the libzmq library.
#
# Its strategy is to:
#  1. Define a set of CMake arguments to pass to the libzmq build system,
#     configuring it to build as a static library with libsodium support.
#  2. Since libzmq uses CMake, this script can rely on the default build
#     behavior of the generic `pylabhub_add_external_prerequisite` function.
#  3. It calls this helper function, passing only the specific CMAKE_ARGS
#     and metadata needed for the build and post-build normalization.
#
# Wrapper for building libzmq using the generic prerequisite helper function.
include(ThirdPartyPolicyAndHelper)

# --- 1. Define Paths ---
if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/libzmq")
set(_build_dir "${CMAKE_BINARY_DIR}/third_party/libzmq-build")
set(_install_dir "${PREREQ_INSTALL_DIR}")

# --- 2. Define CMake Arguments for this specific project ---
# The libzmq project is CMake-based, so we can rely on the helper function's
# default command generation and just pass CMake arguments.
set(_cmake_args
  "-DBUILD_TESTS=OFF"
  "-DZMQ_BUILD_TESTS=OFF"
  "-DBUILD_STATIC=ON"
  "-DBUILD_SHARED=ON"         # Build both, static is preferred by default.
  "-DWITH_PERF_TOOL=OFF"
  "-DWITH_DOCS=OFF"
  "-DENABLE_CURVE=ON"
  "-DWITH_LIBSODIUM=ON"
  "-DSODIUM_INCLUDE_DIRS:PATH=${_install_dir}/include"
  # Point to the stable, normalized path created by the libsodium prerequisite build.
  "-DSODIUM_LIBRARY:FILEPATH=${_install_dir}/lib/libsodium-stable"
)

# Determine byproduct and lib patterns based on platform
if(WIN32)
    set(_byproduct "${_install_dir}/lib/libzmq-mt-s.lib")
    set(_lib_patterns "libzmq-mt-s.lib;libzmq-mt.lib")
else()
    set(_byproduct "${_install_dir}/lib/libzmq.a")
    set(_lib_patterns "libzmq.a;libzmq.so;libzmq.dylib")
endif()


# --- 3. Call the generic helper function ---
pylabhub_add_external_prerequisite(
  NAME            libzmq
  SOURCE_DIR      "${_source_dir}"
  BINARY_DIR      "${_build_dir}"
  INSTALL_DIR     "${_install_dir}"
  DEPENDS         libsodium

  # Pass the project-specific CMake args
  CMAKE_ARGS      ${_cmake_args}

  # Define patterns for the post-build detection script
  LIB_PATTERNS    ${_lib_patterns}
  BUILD_BYPRODUCTS ${_byproduct}
)

# --- 4. Provide convenience alias ---
if(NOT TARGET libzmq::pylabhub)
  add_library(libzmq::pylabhub ALIAS pylabhub::third_party::libzmq)
endif()

message(STATUS "[pylabhub-third-party] libzmq configuration complete.")
