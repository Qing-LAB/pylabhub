# ----------------------------------------------------------------------------
# cmake/XOPToolKit.cmake
# Purpose: define XOPSupport-related third-party build options and defaults.
# ----------------------------------------------------------------------------

# If vendor directory exists at the provided default, prefer it.
set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Vendor XOPSupport tree (preferred when available).")

# Option to enable/disable building the XOP plugin entirely (user-visible)
option(BUILD_XOP "Build the pylabhub XOP plugin (only supported on macOS and Windows x64)" ON)

# This file only exposes the informtion on XOPToolKit which needs to be
# downloaded manually by the user from the official site with permission
# from WaveMetrics.
# No compilation or installation of the XOPToolKit is needed.
# The only thing needed is to point CMake to the location of the
# XOPToolKit on the user's system.
# The use of the XOPToolKit is handled in src/IgorXOP/CMakeLists.txt
# and src/IgorXOP/cmake/FindXOPSupport.cmake.

message(STATUS "=============================================================")
message(STATUS "XOPToolkit / XOPSupport build options:")
message(STATUS "  XOP_VENDOR_DIR=${XOP_VENDOR_DIR}")
message(STATUS "  USE_SYSTEM_XOPSUPPORT=${USE_SYSTEM_XOPSUPPORT}")
message(STATUS "  BUILD_XOP=${BUILD_XOP}")
message(STATUS "=============================================================")