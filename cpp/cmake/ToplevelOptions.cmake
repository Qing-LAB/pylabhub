# -----------------------------------------------------------------------------
# Top-level Options (keep project defaults, add third-party control knobs)
# -----------------------------------------------------------------------------
# Project-owned options (unchanged semantics)
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (Linux/GNU Clang only)" OFF)

option(PYLABHUB_BUILD_SHARED "Build the pylabhub library as SHARED instead of STATIC" OFF)
option(PYLABHUB_INSTALL_HEADERS "Install headers under ${CMAKE_INSTALL_INCLUDEDIR}" ON)
