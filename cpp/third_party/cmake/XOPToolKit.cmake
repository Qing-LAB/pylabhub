include(ThirdPartyPolicyAndHelper) # Ensure helpers are available.
# ----------------------------------------------------------------------------
# third_party/cmake/XOPToolKit.cmake
#
# Purpose:
#   This script finds the proprietary WaveMetrics XOP Toolkit SDK and exposes
#   it as a clean, modern CMake target for the main project to consume.
#
# Responsibilities:
#  - Find the XOP SDK include path and libraries using hints provided by
#    the user (`USE_SYSTEM_XOPSUPPORT`) or a default vendored path.
#  - Create a stable, namespaced `pylabhub::third_party::XOPToolKit` target.
#  - Populate the target's `INTERFACE` properties with the SDK's include
#    directories and link libraries.
#  - This script does NOT stage any XOP SDK files due to licensing constraints.
#
# This script is controlled by the following global options/variables:
#
# - BUILD_XOP: (CACHE BOOL: ON | OFF)
#   If ON, this script will attempt to find the XOP SDK. If it fails, it
#   will produce a fatal error.
#
# - USE_SYSTEM_XOPSUPPORT: (CACHE PATH)
#   A user-provided path to the root of an external XOP SDK installation.
#   This path has the highest priority.
#
# - XOP_VENDOR_DIR: (CACHE PATH)
#   The default path for a vendored copy of the XOP SDK, used as a fallback.
# ----------------------------------------------------------------------------

# --- XOP Build Policy ---
option(BUILD_XOP "Build the pylabhub XOP plugin (only supported on macOS and Windows x64)" ON)

# This variable provides a default path for a vendored copy of the XOP SDK.
set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Default path for a vendored XOPSupport tree.")

# Enforce platform compatibility. The XOP SDK is only for Windows and macOS.
if(BUILD_XOP AND NOT (PLATFORM_WIN64 OR PLATFORM_APPLE))
  message(WARNING "[pylabhub-third-party] BUILD_XOP is ON but the current platform is not supported (Windows x64 or macOS required). Disabling XOP build.")
  set(BUILD_XOP OFF CACHE BOOL "Disabling XOP build on unsupported platform." FORCE)
endif()

if(NOT BUILD_XOP)
  message(STATUS "[pylabhub-third-party] BUILD_XOP is OFF. Skipping XOPToolKit discovery.")
  return()
endif()

message(STATUS "[pylabhub-third-party] Configuring XOPToolKit dependency...")

# --- 1. Find the XOP SDK ---
set(XOP_SDK_HINTS
  "${USE_SYSTEM_XOPSUPPORT}" # User-provided path has highest priority
  "${XOP_VENDOR_DIR}"         # Default vendored path
)

find_path(XOP_SDK_INCLUDE_DIR
  NAMES "XOP.h"
  HINTS ${XOP_SDK_HINTS}
  PATH_SUFFIXES "include" "XOP Support"
)
if(NOT XOP_SDK_INCLUDE_DIR)
  message(FATAL_ERROR "[pylabhub-third-party] Could not find XOPToolKit headers (XOP.h). "
    "Please set USE_SYSTEM_XOPSUPPORT to the root of your XOP SDK installation, "
    "or place the SDK at '${XOP_VENDOR_DIR}'.")
endif()
message(STATUS "[pylabhub-third-party] Found XOP SDK includes at: ${XOP_SDK_INCLUDE_DIR}")

# --- 2. Find the XOP SDK Libraries ---
set(XOP_SDK_LIBRARY "")
set(XOP_SDK_IGOR_LIBRARY "")

if(PLATFORM_WIN64)
  find_library(XOP_SDK_LIBRARY NAMES XOPSupport64 HINTS ${XOP_SDK_HINTS} PATH_SUFFIXES "VC/x64" "VC/x64/Release" "lib64" "lib")
  find_library(XOP_SDK_IGOR_LIBRARY NAMES IGOR64 HINTS ${XOP_SDK_HINTS} PATH_SUFFIXES "VC/x64" "VC/x64/Release" "lib64" "lib")
elseif(PLATFORM_APPLE)
  find_library(XOP_SDK_LIBRARY NAMES XOPSupport64 libXOPSupport64 HINTS ${XOP_SDK_HINTS} PATH_SUFFIXES "Xcode" "Xcode/Release" "lib64" "lib")
endif()

if(NOT XOP_SDK_LIBRARY)
    message(FATAL_ERROR "[pylabhub-third-party] Could not find the XOPToolKit library (e.g., XOPSupport64.lib). "
    "Please ensure it exists in your SDK installation.")
endif()

message(STATUS "[pylabhub-third-party] Found XOP SDK library at: ${XOP_SDK_LIBRARY}")
if(XOP_SDK_IGOR_LIBRARY)
  message(STATUS "[pylabhub-third-party] Found IGOR import library at: ${XOP_SDK_IGOR_LIBRARY}")
endif()

# --- 3. Create the wrapper target ---
_expose_wrapper(pylabhub_xoptoolkit pylabhub::third_party::XOPToolKit)

# --- 4. Populate usage requirements ---
target_include_directories(pylabhub_xoptoolkit INTERFACE
  $<BUILD_INTERFACE:${XOP_SDK_INCLUDE_DIR}>
  # No INSTALL_INTERFACE as this is a compile-time only dependency.
)

# Link the found SDK library to the wrapper target.
target_link_libraries(pylabhub_xoptoolkit INTERFACE "${XOP_SDK_LIBRARY}")

# Also link the IGOR import library if it was found (Windows).
if(XOP_SDK_IGOR_LIBRARY)
  target_link_libraries(pylabhub_xoptoolkit INTERFACE "${XOP_SDK_IGOR_LIBRARY}")
endif()

# Add platform-specific framework/system library dependencies to the wrapper.
# This moves the logic out of the IgorXOP build script and into the dependency definition.
if(PLATFORM_APPLE)
  target_link_libraries(pylabhub_xoptoolkit INTERFACE
    "-framework Cocoa"          # Required by XOP Toolkit for UI elements
    "-framework CoreFoundation" # Core services
    "-framework CoreServices"   # Core services
    "-framework Carbon"         # Legacy UI components still used by Igor Pro
    "-framework AudioToolbox"   # For system sounds, etc.
  )
  message(STATUS "[pylabhub-third-party] Added macOS frameworks to XOPToolKit target.")
elseif(PLATFORM_WIN64)
  target_link_libraries(pylabhub_xoptoolkit INTERFACE version)
  message(STATUS "[pylabhub-third-party] Added 'version' library to XOPToolKit target for Windows.")
endif()

message(STATUS "[pylabhub-third-party] XOPToolKit configuration complete.")