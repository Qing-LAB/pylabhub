# --------------------------------------------
# third_party/cmake/nlohmann_json.cmake
# Setup nlohmann/json
# Create an INTERFACE wrapper target 'pylabhub_nlohmann_json'
# and a namespaced alias 'pylabhub::nlohmann_json' that forwards
# to either:
#   - vendored headers (preferred when PREFER_VENDOR_NLOHMANN=ON), or
#   - an upstream target (if provided by find_package or FetchContent),
#   - or a FetchContent-provided source tree include dir.
# --------------------------------------------
message("==================================================================")
message("third_party: Setting up nlohmann/json")
message("==================================================================")
if(NOT DEFINED PREFER_VENDOR_NLOHMANN)
  set(PREFER_VENDOR_NLOHMANN OFF)
endif()

# canonical variables used locally
set(_nlohmann_include_dir "")
set(_nlohmann_target_created OFF)
set(_nlohmann_target_name "")

# Ensure our local wrapper target exists (concrete target, no '::' in name)
if(NOT TARGET pylabhub_nlohmann_json)
  add_library(pylabhub_nlohmann_json INTERFACE)
endif()

# 1) Try vendored header path first (explicit vendored copy)
if(PREFER_VENDOR_NLOHMANN)
  set(_candidate_header "${CMAKE_CURRENT_SOURCE_DIR}/include/nlohmann/json.hpp")
  if(EXISTS "${_candidate_header}")
    get_filename_component(_nlohmann_include_dir "${_candidate_header}" DIRECTORY)
    message(STATUS "third_party: using vendored nlohmann/json headers at ${_nlohmann_include_dir}")

    target_include_directories(pylabhub_nlohmann_json INTERFACE
      $<BUILD_INTERFACE:${_nlohmann_include_dir}>
      $<INSTALL_INTERFACE:include>
    )

    set(_nlohmann_target_created ON)
    set(_nlohmann_target_name "pylabhub_nlohmann_json")
    message(STATUS "third_party: created vendored pylabhub_nlohmann_json INTERFACE -> ${_nlohmann_include_dir}")
  else()
    message(STATUS "third_party: vendored nlohmann/json requested but header not found at ${_candidate_header}")
  endif()
endif()

# 2) If not vendored (or vendored absent), try FetchContent / upstream target
if(NOT _nlohmann_target_created)
  include(FetchContent)

  message(STATUS "third_party: attempting to use upstream nlohmann/json or FetchContent fallback")

  # If a find_package or other mechanism already provided namespaced target,
  # prefer it. We will not alias directly to that target; instead we link our
  # local wrapper to it (avoids alias-of-alias).
  if(TARGET nlohmann_json::nlohmann_json)
    message(STATUS "third_party: found upstream target nlohmann_json::nlohmann_json")
    target_link_libraries(pylabhub_nlohmann_json INTERFACE nlohmann_json::nlohmann_json)
    set(_nlohmann_target_created ON)
    set(_nlohmann_target_name "nlohmann_json::nlohmann_json")
  elseif(TARGET nlohmann_json)
    message(STATUS "third_party: found upstream target nlohmann_json")
    target_link_libraries(pylabhub_nlohmann_json INTERFACE nlohmann_json)
    set(_nlohmann_target_created ON)
    set(_nlohmann_target_name "nlohmann_json")
  else()
    # FetchContent fallback - header-only
    message(STATUS "third_party: FetchContent'ing nlohmann/json (header-only)")
    FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.11.2
    )
    FetchContent_MakeAvailable(nlohmann_json)

    # If the fetch produced an exported target, link to it; otherwise set include dir.
    if(TARGET nlohmann_json::nlohmann_json)
      message(STATUS "third_party: FetchContent created target nlohmann_json::nlohmann_json")
      target_link_libraries(pylabhub_nlohmann_json INTERFACE nlohmann_json::nlohmann_json)
      set(_nlohmann_target_created ON)
      set(_nlohmann_target_name "nlohmann_json::nlohmann_json")
    elseif(TARGET nlohmann_json)
      message(STATUS "third_party: FetchContent created target nlohmann_json")
      target_link_libraries(pylabhub_nlohmann_json INTERFACE nlohmann_json)
      set(_nlohmann_target_created ON)
      set(_nlohmann_target_name "nlohmann_json")
    elseif(DEFINED nlohmann_json_SOURCE_DIR)
      # try to locate headers in the fetched source tree
      set(_maybe_include_dir "${nlohmann_json_SOURCE_DIR}/include")
      if(EXISTS "${_maybe_include_dir}/nlohmann/json.hpp" OR EXISTS "${_maybe_include_dir}/json.hpp")
        set(_nlohmann_include_dir "${_maybe_include_dir}")
        target_include_directories(pylabhub_nlohmann_json INTERFACE
          $<BUILD_INTERFACE:${_nlohmann_include_dir}>
          $<INSTALL_INTERFACE:include>
        )
        message(STATUS "third_party: created pylabhub_nlohmann_json from fetched source -> ${_nlohmann_include_dir}")
        set(_nlohmann_target_created ON)
        set(_nlohmann_target_name "pylabhub_nlohmann_json")
      else()
        message(WARNING "third_party: fetched nlohmann_json but could not find headers under ${_maybe_include_dir}")
      endif()
    else()
      message(WARNING "third_party: FetchContent made available nlohmann_json but ${nlohmann_json_SOURCE_DIR} not set")
    endif()
  endif()
endif()

# 3) Expose namespaced alias to consumers: pylabhub::nlohmann_json
# Always create alias to our local wrapper target (pylabhub_nlohmann_json).
# This ensures downstream code uses a stable name and avoids alias-of-alias problems.
if(NOT TARGET pylabhub::nlohmann_json)
  add_library(pylabhub::nlohmann_json ALIAS pylabhub_nlohmann_json)
  message(STATUS "third_party: created alias pylabhub::nlohmann_json -> pylabhub_nlohmann_json")
else()
  message(STATUS "third_party: pylabhub::nlohmann_json already exists")
endif()

# 4) Export a canonical name/value to the parent
if(_nlohmann_target_created)
  # prefer exposing the namespaced alias as the canonical target
  set(NLOHMANN_TARGET "pylabhub::nlohmann_json" PARENT_SCOPE)
  set(NLOHMANN_REAL_TARGET "${_nlohmann_target_name}" PARENT_SCOPE)
  message(STATUS "third_party: nlohmann/json setup complete")
  message(STATUS "    target NLOHMANN_TARGET = ${NLOHMANN_TARGET}")
  message(STATUS "    real target NLOHMANN_REAL_TARGET: ${NLOHMANN_REAL_TARGET}")
else()
  set(NLOHMANN_TARGET "" PARENT_SCOPE)
  set(NLOHMANN_REAL_TARGET "" PARENT_SCOPE)
  message(WARNING "third_party: nlohmann/json setup failed - no target created")
endif()

# 5) Install headers (only when include dir determined and install enabled)
if(THIRD_PARTY_INSTALL AND _nlohmann_target_created AND _nlohmann_include_dir AND EXISTS "${_nlohmann_include_dir}")
  if(EXISTS "${_nlohmann_include_dir}/json.hpp")
    install(FILES "${_nlohmann_include_dir}/json.hpp" DESTINATION include)
  elseif(EXISTS "${_nlohmann_include_dir}/nlohmann/json.hpp")
    install(DIRECTORY "${_nlohmann_include_dir}/nlohmann" DESTINATION include)
  endif()

  message("")
  message("=========================================================")
  message(STATUS "INSTALL setup nlohmann/json headers")
  message(STATUS "third_party: nlohmann/json install enabled")
  message(STATUS "third_party: nlohmann/json headers will be installed from ${_nlohmann_include_dir}")
  message(STATUS "third_party: nlohmann/json target = pylabhub::nlohmann_json")
  message("=========================================================")
  message("")
endif()
