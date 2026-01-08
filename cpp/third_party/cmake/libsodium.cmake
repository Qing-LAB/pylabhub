# ---------------------------------------------------------------------------
# third_party/cmake/libsodium.cmake
# Wrapper for libsodium, which is an Autotools-based project.
#
# This script uses ExternalProject_Add to configure, build, and install
# libsodium into a temporary location within our build directory. It then
# creates an IMPORTED library target that can be used by other CMake targets
# within this project.
#
# To support dependent projects like libzmq, it exports the installation
# directory path to the `PYLABHUB_LIBSODIUM_ROOT_DIR` cache variable.
# ---------------------------------------------------------------------------
include(ExternalProject)
include(ThirdPartyPolicyAndHelper)

find_package(Git REQUIRED)

set(LIBSODIUM_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libsodium")
set(LIBSODIUM_BUILD_DIR "${CMAKE_BINARY_DIR}/third_party/libsodium-build")
set(LIBSODIUM_INSTALL_DIR "${CMAKE_BINARY_DIR}/third_party/libsodium-install")

# On Windows, we need to find `sh.exe` for the configure script.
# This is typically available if Git for Windows is installed.
if(WIN32)
  if(NOT DEFINED ENV{PATH})
    set(ENV{PATH} "")
  endif()
  # Find the directory containing sh.exe
  find_program(SH_EXECUTABLE
    NAMES sh.exe
    PATHS "C:/Program Files/Git/usr/bin" "C:/Program Files (x86)/Git/usr/bin"
    ENV PATH
  )
  if(NOT SH_EXECUTABLE)
    message(FATAL_ERROR "sh.exe not found. Please install Git for Windows and ensure its bin directory is in your PATH.")
  else()
    message(STATUS "[pylabhub-third-party] Found sh.exe at: ${SH_EXECUTABLE}")
  endif()
  set(CONFIGURE_COMMAND ${SH_EXECUTABLE} ${LIBSODIUM_SOURCE_DIR}/configure)
else()
  set(CONFIGURE_COMMAND ${LIBSODIUM_SOURCE_DIR}/configure)
endif()

ExternalProject_Add(
  libsodium_external
  SOURCE_DIR ${LIBSODIUM_SOURCE_DIR}
  BINARY_DIR ${LIBSODIUM_BUILD_DIR}
  INSTALL_DIR ${LIBSODIUM_INSTALL_DIR}

  # Configure Step: Build as a static library, disable things we don't need.
  CONFIGURE_COMMAND ${CONFIGURE_COMMAND}
    --prefix=${LIBSODIUM_INSTALL_DIR}
    --disable-shared
    --enable-static
    --disable-tests
    --disable-dependency-tracking
    --with-pic # Position Independent Code is crucial for linking into other libs

  # Build & Install Steps
  BUILD_COMMAND $(MAKE)
  INSTALL_COMMAND $(MAKE) install

  # Ensure the built library is available for other targets
  BUILD_BYPRODUCTS ${LIBSODIUM_INSTALL_DIR}/lib/libsodium.a
)

# Use ExternalProject_Get_Property to get the definitive install directory.
# This path is then exported as a cache variable to be used as a hint for
# find_package(Sodium) in other projects (like libzmq).
ExternalProject_Get_Property(libsodium_external install_dir)
set(PYLABHUB_LIBSODIUM_ROOT_DIR ${install_dir}
    CACHE INTERNAL "Root directory for pylabhub's libsodium build"
)

# Create an IMPORTED library target. This is what other targets will link against.
add_library(pylabhub::third_party::sodium STATIC IMPORTED GLOBAL)
set_target_properties(pylabhub::third_party::sodium PROPERTIES
  IMPORTED_LOCATION "${install_dir}/lib/libsodium.a"
  INTERFACE_INCLUDE_DIRECTORIES "${install_dir}/include"
)

# Make sure the external project is built before this target is used.
add_dependencies(pylabhub::third_party::sodium libsodium_external)

message(STATUS "[pylabhub-third-party] Configured libsodium external project.")
message(STATUS "[pylabhub-third-party]   - Source: ${LIBSODIUM_SOURCE_DIR}")
message(STATUS "[pylabhub-third-party]   - Install: ${install_dir}")
message(STATUS "[pylabhub-third-party]   - Library: ${install_dir}/lib/libsodium.a")
message(STATUS "[pylabhub-third-party]   - Exporting libsodium root for other projects: ${PYLABHUB_LIBSODIUM_ROOT_DIR}")

if(THIRD_PARTY_INSTALL)
  message(STATUS "[pylabhub-third-party] Scheduling libsodium artifacts for staging...")

  # Stage the static library
  pylabhub_stage_libraries(TARGETS pylabhub::third_party::sodium)

  # Stage the header files
  pylabhub_stage_headers(
    DIRECTORIES "${install_dir}/include"
    SUBDIR "sodium"
  )
endif()
