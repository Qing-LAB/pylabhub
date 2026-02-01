# third_party/cmake/luajit.cmake
#
# This script defines the ExternalProject_Add target for building LuaJIT.
# It follows the project's "Prerequisite Build System" pattern, where the library
# is built externally and installed into a sandboxed `prereqs` directory.
#
# The integration of this pre-built artifact (e.g., creating an IMPORTED
# target and registering it for staging) is handled by the parent
# `third_party/CMakeLists.txt` file.
#
include(ExternalProject)
include(ThirdPartyPolicyAndHelper)

if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

# --- 1. LuaJIT External Project Definition ---
set(LUAJIT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/luajit")
set(LUAJIT_BUILD_DIR "${CMAKE_BINARY_DIR}/third_party/luajit-build")
set(LUAJIT_INSTALL_DIR "${PREREQ_INSTALL_DIR}")

# Determine platform-specific build commands for LuaJIT
if(MSVC)
  # For MSVC, LuaJIT typically uses NMAKE or Visual Studio project files.
  # We will use the provided Visual Studio project for simplicity if available.
  # Assuming 64-bit build.
  # This might require manual adjustments if the project structure changes
  # or if specific VS versions are needed.
  message(STATUS "[pylabhub-third-party] Configuring LuaJIT for MSVC build...")

  # LuaJIT's Visual Studio project files are usually under src.
  # Need to check the exact path to the solution/project file.
  # For now, let's assume a NMAKE build or a custom build step is needed.
  # A more robust solution might involve creating a custom VS project for LuaJIT.

  # As a fallback, we can try to use nmake, but it's more complex.
  # Let's try to adapt the Unix Makefile for mingw/msys2 or custom build.
  # A simpler approach: pre-built binaries or a more complex ExternalProject_Add for VS.
  # Given the complexity, let's assume `make` can be used with `MinGW` or `msys2` for now,
  # or skip MSVC for initial integration and add later.
  # For the purpose of this exercise, we will configure for non-MSVC first.
  # If MSVC is strictly required, I would investigate LuaJIT's VS build system (or lack thereof) further.

  # For now, FATAL_ERROR if MSVC is detected to prompt for more specific instructions
  # or to indicate that MSVC build for LuaJIT needs more investigation.
  message(FATAL_ERROR "[pylabhub-third-party] LuaJIT integration for MSVC is not yet implemented in this script. "
                      "Manual configuration or a different build approach is required for Windows.")
else()
  # macOS/Linux: standard Makefile build
  message(STATUS "[pylabhub-third-party] Configuring LuaJIT for POSIX build (Makefile)...")

  ExternalProject_Add(luajit_external
    SOURCE_DIR          "${LUAJIT_SOURCE_DIR}"
    BUILD_IN_SOURCE     1 # LuaJIT builds in-source by default
    INSTALL_DIR         "${LUAJIT_INSTALL_DIR}" # Install to the prerequisite dir

    CONFIGURE_COMMAND   "" # No configure step needed for LuaJIT Makefile
    BUILD_COMMAND       make -C "${LUAJIT_SOURCE_DIR}" CFLAGS="${CMAKE_C_FLAGS} -fPIC"
    INSTALL_COMMAND     make -C "${LUAJIT_SOURCE_DIR}" install INSTALL_LDIR="${LUAJIT_INSTALL_DIR}/lib" INSTALL_JROOT="${LUAJIT_INSTALL_DIR}"
    
    # Specify files that should exist after install
    BUILD_BYPRODUCTS    "${LUAJIT_INSTALL_DIR}/lib/libluajit-5.1.a" # Static library
                        "${LUAJIT_INSTALL_DIR}/include/luajit-5.1/lua.h" # A header file

    # Dependency for build_prerequisites
    # No external dependencies for LuaJIT itself, but it is part of overall prereqs.
  )
endif()

# Add the external project to the global prerequisite hook target.
add_dependencies(build_prerequisites luajit_external)

message(STATUS "[pylabhub-third-party] Defined ExternalProject 'luajit_external' to install to ${LUAJIT_INSTALL_DIR}")
