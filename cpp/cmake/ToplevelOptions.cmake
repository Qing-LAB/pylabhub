# -----------------------------------------------------------------------------
# Top-level Options (keep project defaults, add third-party control knobs)
# -----------------------------------------------------------------------------
# Project-owned options (unchanged semantics)
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_SANITIZERS "Enable Address/Undefined sanitizers (Linux/GNU Clang only)" OFF)
option(PREFER_VENDOR_NLOHMANN "Prefer vendored nlohmann/json under third_party/include if present" ON)

option(PYLABHUB_BUILD_SHARED "Build the pylabhub library as SHARED instead of STATIC" OFF)
option(PYLABHUB_INSTALL_HEADERS "Install headers under ${CMAKE_INSTALL_INCLUDEDIR}" ON)


# ----------------------------------------------------------------------------
# XOPToolkit / XOPSupport build options
# ----------------------------------------------------------------------------
# Control whether we attempt an XOP build at all:
option(BUILD_XOP "Attempt to build Igor XOP addon (only supported on Windows x64 and macOS)" ON)
# Allow override from command line:
option(USE_SYSTEM_XOP "Give the directory of system-installed XOP (headers/libs) instead of third_party vendor" "")
# Default vendor path (can be overridden via -DXOP_VENDOR_DIR=/path/to/XOPSupport)
set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Local XOPSupport tree")
