# cmake/ToplevelOptions.cmake
#
# Defines the primary, user-facing options for building the pyLabHub project.
# These options control which components are built and how they are configured.
# This module is included by the top-level CMakeLists.txt.

# Option to build the test suite.
option(BUILD_TESTS "Build the pyLabHub test suite" ON)

# Option to build the IgorXOP module.
option(BUILD_XOP "Build the Igor Pro XOP module" ON)

# Option to stage third-party headers and libraries.
option(THIRD_PARTY_INSTALL "Install third-party libraries and headers to the staging directory" ON)

# Option to stage third-party headers and libraries.
option(THIRD_PARTY_INSTALL "Install third-party libraries and headers to the staging directory" ON)

# Option to generate the final 'install' target.
option(PYLABHUB_CREATE_INSTALL_TARGET "Enable the 'install' target to copy the staged directory" ON)

# Option to enforce the use of Apple's clang toolchain on macOS.
option(FORCE_USE_CLANG_ON_APPLE "Force clang on macOS hosts to avoid conflicts with other compilers" ON)