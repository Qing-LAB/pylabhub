# ---------------------------------------------------------------------------
# Third-party policy knobs (these are wrapper *intent* variables)
# - These inform third_party/CMakeLists.txt which will map them to the exact
#   subproject options (fmt, libzmq, etc.). The wrapper will scope these
#   settings to subprojects so they do not leak to other libs or top-level.
# ---------------------------------------------------------------------------
# Force libzmq variant: "none" | "static" | "shared"
set(THIRD_PARTY_ZMQ_FORCE_VARIANT "static" CACHE STRING "Wrapper intent: force libzmq build variant" FORCE)

# Force fmt variant: "none" | "static" | "shared"
set(THIRD_PARTY_FMT_FORCE_VARIANT "static" CACHE STRING "Wrapper intent: force fmt build variant" FORCE)

# Wrapper-level intent to disable third-party tests by default (ON/OFF).
# The wrapper will set subproject-specific test knobs (e.g. ZMQ_BUILD_TESTS, FMT_TEST)
# rather than changing global BUILD_TESTS.
set(THIRD_PARTY_DISABLE_TESTS ON CACHE BOOL "Wrapper intent: disable third-party tests by default" FORCE)

# Wrapper-level intent on allowing upstream precompiled headers (OFF by default)
set(THIRD_PARTY_ALLOW_UPSTREAM_PCH OFF CACHE BOOL "Wrapper intent: allow upstream precompiled headers" FORCE)

message(STATUS "Top-level third-party policy: THIRD_PARTY_ZMQ_FORCE_VARIANT=${THIRD_PARTY_ZMQ_FORCE_VARIANT}, THIRD_PARTY_FMT_FORCE_VARIANT=${THIRD_PARTY_FMT_FORCE_VARIANT}, THIRD_PARTY_DISABLE_TESTS=${THIRD_PARTY_DISABLE_TESTS}")


# ----------------------------------------------------------------------------
# XOPToolkit / XOPSupport build options
# ----------------------------------------------------------------------------
# Prefer the project vendor copy; override by setting USE_SYSTEM_XOPSUPPORT to a path.
# Example to force system XOPSupport:
#   cmake -D USE_SYSTEM_XOPSUPPORT="/opt/XOPSupport" ...
#
# If vendor directory exists at the provided default, prefer it.
set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Vendor XOPSupport tree (preferred when available).")
# Single override path: if non-empty, FindXOPSupport will use this path and bypass vendor.
set(USE_SYSTEM_XOPSUPPORT "" CACHE PATH "Optional: path to system-installed XOPSupport (overrides vendor tree when set).")

# Option to enable/disable building the XOP plugin entirely (user-visible)
option(BUILD_XOP "Build the pylabhub XOP plugin (only supported on macOS and Windows x64)" ON)

