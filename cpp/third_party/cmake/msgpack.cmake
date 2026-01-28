include(ThirdPartyPolicyAndHelper) # Ensure helpers are available.
include(StageHelpers)

message(STATUS "[pylabhub-third-party] Configuring msgpack-c...")

# This project uses the vendored msgpack-c submodule.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/msgpack-c/include/msgpack.hpp")
    message(FATAL_ERROR "[pylabhub-third-party] Vendored msgpack-c not found. "
                        "Please ensure the git submodule is initialized and updated: "
                        "'git submodule update --init --recursive'")
endif()

# --- 1. Create the wrapper and alias ---
_expose_wrapper(pylabhub_msgpackc pylabhub::third_party::msgpackc)

# --- 2. Define usage requirements ---
# The msgpack-c C++ library is header-only. We just need to add its
# include directory to the wrapper target.
set(_msgpack_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/msgpack-c/include")

target_include_directories(pylabhub_msgpackc INTERFACE
  $<BUILD_INTERFACE:${_msgpack_include_dir}>
)
message(STATUS "[pylabhub-third-party] Configured pylabhub_msgpackc with include directory: ${_msgpack_include_dir}")

# --- 3. Stage artifacts for installation ---
if(THIRD_PARTY_INSTALL)
  # Stage the entire 'msgpack-c/include' directory.
  pylabhub_register_headers_for_staging(
    DIRECTORIES "${_msgpack_include_dir}"
    SUBDIR ""  # This will copy 'msgpack.hpp' and the 'msgpack' subdirectory.
  )
  message(STATUS "[pylabhub-third-party] Staging msgpack-c headers.")
else()
  message(STATUS "[pylabhub-third-party] THIRD_PARTY_INSTALL is OFF; skipping staging for msgpack-c.")
endif()

# --- 4. Add to export set for installation ---
install(TARGETS pylabhub_msgpackc
  EXPORT pylabhubTargets
)

message(STATUS "[pylabhub-third-party] msgpack-c configuration complete.")
