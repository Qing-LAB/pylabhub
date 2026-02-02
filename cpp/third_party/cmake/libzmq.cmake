# third_party/cmake/libzmq.cmake - wrapper for building libzmq via helper macro
include(ThirdPartyPolicyAndHelper)

if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/libzmq")
set(_build_dir "${CMAKE_BINARY_DIR}/third_party/libzmq-build")
set(_install_dir "${PREREQ_INSTALL_DIR}")

# sodium library path depending on platform (point at the actual produced artifact)
if(MSVC)
  # MSVC will produce libsodium.lib
  set(_sodium_lib "${PREREQ_INSTALL_DIR}/lib/libsodium.lib")
else()
  # POSIX static archive
  set(_sodium_lib "${PREREQ_INSTALL_DIR}/lib/libsodium.a")
endif()

# Add project using the helper macro.
# Explicitly pass absolute CMAKE_INSTALL_PREFIX and ensure tests are disabled.
pylabhub_add_external_prerequisite(
  NAME libzmq
  SOURCE_DIR "${_source_dir}"
  BINARY_DIR "${_build_dir}"
  INSTALL_DIR "${_install_dir}"
  DEPENDS libsodium

  # Provide explicit install prefix so the downstream project installs into staging.
  # Also disable testing using both libzmq-specific and generic flags.
  CMAKE_ARGS
    "-DCMAKE_INSTALL_PREFIX:PATH=${_install_dir}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DBUILD_TESTS=OFF"        # generic CMake tests switch
    "-DZMQ_BUILD_TESTS=OFF"      # libzmq-specific tests switch
    "-DBUILD_STATIC=ON"
    "-DBUILD_SHARED=ON"         # WORKAROUND for libzmq internal build issue
    "-DWITH_PERF_TOOL=OFF"
    "-DWITH_DOCS=OFF"
    "-DENABLE_CURVE=ON"
    "-DWITH_LIBSODIUM=ON"
    "-DSODIUM_INCLUDE_DIRS:PATH=${PREREQ_INSTALL_DIR}/include"
    "-DSODIUM_LIBRARIES:FILEPATH=${_sodium_lib}"
)

# Provide helpful alias target (optional)
if(NOT TARGET libzmq::pylabhub)
  add_library(libzmq::pylabhub ALIAS pylabhub::third_party::libzmq)
endif()
