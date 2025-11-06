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
