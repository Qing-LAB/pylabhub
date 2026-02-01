include(ThirdPartyPolicyAndHelper) # Ensure helpers are available.
include(StageHelpers)

message(STATUS "[pylabhub-third-party] Configuring cppzmq...")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cppzmq/zmq.hpp")
    message(FATAL_ERROR "[pylabhub-third-party] Vendored cppzmq not found. "
                        "Please ensure the git submodule is initialized and updated: "
                        "'git submodule update --init --recursive'")
endif()

# --- 1. Create the wrapper and alias ---
_expose_wrapper(pylabhub_cppzmq pylabhub::third_party::cppzmq)

# --- 2. Define usage requirements ---
set(_cppzmq_include_dir "${CMAKE_CURRENT_SOURCE_DIR}")

target_include_directories(pylabhub_cppzmq INTERFACE
  $<BUILD_INTERFACE:${_cppzmq_include_dir}>
)

# cppzmq depends on libzmq. We need to link against the pylabhub wrapper for libzmq.
target_link_libraries(pylabhub_cppzmq INTERFACE pylabhub::third_party::libzmq)

message(STATUS "[pylabhub-third-party] Configured pylabhub_cppzmq with include directory: ${_cppzmq_include_dir}")
message(STATUS "[pylabhub-third-party] Linking pylabhub_cppzmq -> pylabhub::third_party::libzmq")


# --- 3. Stage artifacts for installation ---
if(THIRD_PARTY_INSTALL)
  # Register the specific cppzmq headers for staging using the new FILES argument.
  pylabhub_register_headers_for_staging(
    FILES
      "${_cppzmq_include_dir}/cppzmq/zmq.hpp"
      "${_cppzmq_include_dir}/cppzmq/zmq_addon.hpp"
    SUBDIR "cppzmq"
  )
  message(STATUS "[pylabhub-third-party] Staging cppzmq headers (zmq.hpp, zmq_addon.hpp).")
else()
  message(STATUS "[pylabhub-third-party] THIRD_PARTY_INSTALL is OFF; skipping staging for cppzmq.")
endif()

# --- 4. Add to export set for installation ---
install(TARGETS pylabhub_cppzmq
  EXPORT pylabhubTargets
)

message(STATUS "[pylabhub-third-party] cppzmq configuration complete.")
