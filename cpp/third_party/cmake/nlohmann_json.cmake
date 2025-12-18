include(ThirdPartyPolicyAndHelper) # Ensure helpers are available.
# --------------------------------------------
# third_party/cmake/nlohmann_json.cmake
# Setup nlohmann/json
#
# Responsibilities:
#  - Discover the nlohmann/json headers using a priority system:
#    1. A local vendored copy, if `PREFER_VENDOR_NLOHMANN` is ON.
#    2. An existing upstream target (from `find_package` or a parent `FetchContent`).
#    3. A `FetchContent` download as a final fallback.
#  - Provide a stable, namespaced `pylabhub::third_party::nlohmann_json` target.
#  - Stage the header files into the project's `PYLABHUB_STAGING_DIR` by
#    attaching a custom command to the `stage_third_party_deps` target.
# --------------------------------------------
#
# This script is controlled by the following global options/variables:
#
# - PREFER_VENDOR_NLOHMANN: (CACHE BOOL: ON | OFF)
#   If ON, the script will prioritize using the headers from the local
#   `third_party/include/nlohmann` directory.
#
# - THIRD_PARTY_INSTALL: (CACHE BOOL: ON | OFF)
#   If ON, enables the post-build staging of the nlohmann/json headers.
#
# The following are provided by the top-level build environment and are not
# intended to be user-configurable options:
#
# - PYLABHUB_STAGING_DIR: (PATH)
#   The absolute path to the staging directory where artifacts will be copied.
#
# - stage_third_party_deps: (CMake Target)
#   The custom target "hook" to which staging commands are attached.
# --------------------------------------------

# Include the top-level staging helpers.
include(StageHelpers)

message(STATUS "[pylabhub-third-party] Configuring nlohmann/json...")

if(NOT DEFINED PREFER_VENDOR_NLOHMANN)
  set(PREFER_VENDOR_NLOHMANN OFF)
endif()

# Local bookkeeping
set(_nlohmann_include_dir "")        # resolved include dir when using header copy (should point to parent 'include' dir)
set(_nlohmann_upstream_target "")    # the 'real' underlying target name (if any)
set(_nlohmann_layout "unknown")      # "namespaced" or "flat" or "unknown" (for messaging)

# --- 1. Create the wrapper and alias ---
# This provides the stable, namespaced target for consumers.
_expose_wrapper(pylabhub_nlohmann_json pylabhub::third_party::nlohmann_json)

# --- Local Helper Function ---
# Checks for the existence of the canonical nlohmann_json targets and sets
# _nlohmann_upstream_target if one is found. This centralizes the discovery logic.
function(_discover_nlohmann_target)
  if(TARGET nlohmann_json::nlohmann_json)
    set(_nlohmann_upstream_target "nlohmann_json::nlohmann_json" PARENT_SCOPE)
  elseif(TARGET nlohmann_json)
    set(_nlohmann_upstream_target "nlohmann_json" PARENT_SCOPE)
  else()
    # No target found, ensure the variable is cleared.
    set(_nlohmann_upstream_target "" PARENT_SCOPE)
  endif()
endfunction()


# --- 2. Discover the nlohmann/json source ---

# Priority 1: Use local vendored copy if requested.
if(PREFER_VENDOR_NLOHMANN AND NOT _nlohmann_upstream_target)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/nlohmann/json.hpp")
    set(_nlohmann_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set(_nlohmann_layout "namespaced")
    message(STATUS "[pylabhub-third-party] Found vendored nlohmann/json (namespaced layout).")
  # Design Rationale: For a vendored copy, we enforce a namespaced layout.
  # If a flat layout is detected (json.hpp directly in include/), we fail fast.
  # This is a strict policy that requires the developer to fix the vendored
  # source tree to match the project's expectations (`#include <nlohmann/json.hpp>`).
  # This is a one-time fix that ensures consistency.
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/json.hpp")
    message(FATAL_ERROR "[pylabhub-third-party] Found vendored nlohmann/json with a 'flat' layout. "
                        "This project requires a 'namespaced' layout. Please move 'include/json.hpp' "
                        "to 'include/nlohmann/json.hpp'.")
  else()
    message(STATUS "[pylabhub-third-party] PREFER_VENDOR_NLOHMANN=ON but no headers found in `${CMAKE_CURRENT_SOURCE_DIR}/include`.")
  endif()
endif()

# Priority 2: Check for an existing upstream target (from find_package or parent FetchContent).
if(NOT _nlohmann_include_dir)
  _discover_nlohmann_target()
  if(_nlohmann_upstream_target)
      message(STATUS "[pylabhub-third-party] Found existing upstream target: ${_nlohmann_upstream_target}")
  endif()
endif()

# Priority 3: Fallback to FetchContent if nothing has been found yet.
if(NOT _nlohmann_include_dir AND NOT _nlohmann_upstream_target)
  include(FetchContent) # safe to include even if find_package was used earlier
  message(STATUS "[pylabhub-third-party] No vendored or existing target found. Fetching nlohmann/json...")
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    # Pick a stable tag you trust; update as needed at the project level.
    GIT_TAG v3.11.2
  )
  FetchContent_MakeAvailable(nlohmann_json)

  # After fetching, check again for the target it created.
  _discover_nlohmann_target()
  if(_nlohmann_upstream_target)
      message(STATUS "[pylabhub-third-party] FetchContent provided target: ${_nlohmann_upstream_target}")
      # Design Rationale: Some older versions of nlohmann/json fetched via
      # FetchContent might have a "flat" header layout. We detect this and
      # normalize it by creating a namespaced `nlohmann` subdirectory within
      # the build tree's source directory for this dependency. This allows the
      # rest of our project to consistently use `#include <nlohmann/json.hpp>`
      # without modifying the original downloaded source.
      # Check the layout of the fetched content and fix it if it's flat.
      if(EXISTS "${nlohmann_json_SOURCE_DIR}/include/json.hpp" AND NOT EXISTS "${nlohmann_json_SOURCE_DIR}/include/nlohmann")
        message(STATUS "[pylabhub-third-party] Fetched content has a flat layout. Normalizing to namespaced layout for this build.")
        file(MAKE_DIRECTORY "${nlohmann_json_SOURCE_DIR}/include/nlohmann")
        # Note: This copy happens in the build tree, not the original source.
        file(COPY "${nlohmann_json_SOURCE_DIR}/include/json.hpp" DESTINATION "${nlohmann_json_SOURCE_DIR}/include/nlohmann/")
      endif()
      # Now that the layout is guaranteed to be namespaced, set the include directory.
      set(_nlohmann_include_dir "${nlohmann_json_SOURCE_DIR}/include")
      set(_nlohmann_layout "namespaced")
  else()
      # This case is unlikely but handled for robustness.
      if(DEFINED nlohmann_json_SOURCE_DIR)
          set(_nlohmann_include_dir "${nlohmann_json_SOURCE_DIR}/include")
          set(_nlohmann_layout "namespaced") # Assume FetchContent provides a namespaced layout.
      endif()
  endif()
endif()

# --- 3. Define usage requirements ---
# Link the wrapper to the upstream target if found, otherwise use the discovered include directory.
if(_nlohmann_upstream_target)
  target_link_libraries(pylabhub_nlohmann_json INTERFACE "${_nlohmann_upstream_target}")
  message(STATUS "[pylabhub-third-party] Linking pylabhub_nlohmann_json -> ${_nlohmann_upstream_target}")

  # If we don't already have a reliable include path (from vendoring or FetchContent),
  # try to get it from the upstream target's properties for staging purposes.
  if(NOT _nlohmann_include_dir)
    get_target_property(maybe_include_dir "${_nlohmann_upstream_target}" INTERFACE_INCLUDE_DIRECTORIES)
    if(maybe_include_dir)
      set(_nlohmann_include_dir "${maybe_include_dir}")
    endif()
  endif()

elseif(_nlohmann_include_dir)
  # This path is used for a vendored library. We've already enforced that the
  # layout must be namespaced, so adding this path is correct.
  target_include_directories(pylabhub_nlohmann_json INTERFACE
    $<BUILD_INTERFACE:${_nlohmann_include_dir}>)
  message(STATUS "[pylabhub-third-party] Configured pylabhub_nlohmann_json with vendored include directory: ${_nlohmann_include_dir}")
else()
  message(FATAL_ERROR "[pylabhub-third-party] Failed to find or configure nlohmann/json. No target or include directory could be resolved.")
endif()

# --- 4. Stage artifacts for installation ---
if(THIRD_PARTY_INSTALL)
  if(_nlohmann_include_dir AND EXISTS "${_nlohmann_include_dir}/nlohmann/json.hpp")
    # The layout is guaranteed to be namespaced at this point. Copy the nlohmann
    # directory from the resolved include path into the staging include directory.
    pylabhub_stage_headers(DIRECTORIES "${_nlohmann_include_dir}/nlohmann" SUBDIR "nlohmann")
    message(STATUS "[pylabhub-third-party] Staging nlohmann/json headers.")
  else()
    message(WARNING "[pylabhub-third-party] No include directory resolved for nlohmann/json; skipping header staging.")
  endif()
else()
    message(STATUS "[pylabhub-third-party] THIRD_PARTY_INSTALL is OFF; skipping staging for nlohmann/json.")
endif()

# --- 5. Add to export set for installation ---
# This target is an INTERFACE library, but it must be part of the export
# set so that downstream projects consuming our package can find its
# include directories.
install(TARGETS pylabhub_nlohmann_json
  EXPORT pylabhubTargets
)

message(STATUS "[pylabhub-third-party] nlohmann/json configuration complete.")
