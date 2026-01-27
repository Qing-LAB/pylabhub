# third_party/cmake/libsodium.cmake
#
# This script uses ExternalProject_Add to build libsodium.
# It is designed to be the first step in a prerequisite build chain.
#
include(ExternalProject)
include(ThirdPartyPolicyAndHelper)

# This will be set in the parent scope (third_party/CMakeLists.txt)
if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

set(LIBSODIUM_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libsodium")
set(LIBSODIUM_INSTALL_DIR "${PREREQ_INSTALL_DIR}") # Install to the prerequisite dir
set(LIBSODIUM_BUILD_DIR "${CMAKE_BINARY_DIR}/third_party/libsodium-build")

if(MSVC)
  # (MSVC-specific logic remains the same, but with LIBSODIUM_INSTALL_DIR changed)
  # ...
else()
  # macOS/Linux: autotools build
  ExternalProject_Add(
    libsodium_external
    SOURCE_DIR   "${LIBSODIUM_SOURCE_DIR}"
    BINARY_DIR   "${LIBSODIUM_BUILD_DIR}"
    INSTALL_DIR  "${LIBSODIUM_INSTALL_DIR}"
    CONFIGURE_COMMAND
      "${CMAKE_COMMAND}" -E env
        "CC=${CMAKE_C_COMPILER}"
        "CXX=${CMAKE_CXX_COMPILER}"
      "${LIBSODIUM_SOURCE_DIR}/configure"
        --prefix=<INSTALL_DIR>
        --disable-shared
        --enable-static
        --disable-tests
        --disable-dependency-tracking
        --with-pic
    BUILD_COMMAND    "$(MAKE)"
    INSTALL_COMMAND  "$(MAKE)" install
    BUILD_BYPRODUCTS "<INSTALL_DIR>/lib/libsodium.a"
  )
endif()

message(STATUS "[pylabhub-third-party] Defined libsodium_external project to install to ${LIBSODIUM_INSTALL_DIR}")