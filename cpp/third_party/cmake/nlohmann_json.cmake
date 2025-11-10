# --------------------------------------------
# third_party/cmake/nlohmann_json.cmake
# Setup nlohmann/json
# Set up an INTERFACE target 'pylabhub::nlohmann_json'
# that either uses a vendored copy or FetchContent.
# --------------------------------------------

if(NOT DEFINED PREFER_VENDOR_NLOHMANN)
  set(PREFER_VENDOR_NLOHMANN OFF)
endif()

set(_nlohmann_include_dir "")
set(_nlohmann_target_created OFF)

# 1) Try vendored header path first (explicit vendored copy)
if(PREFER_VENDOR_NLOHMANN)
  set(_candidate_header "${CMAKE_CURRENT_SOURCE_DIR}/third_party/include/nlohmann/json.hpp")
  if(EXISTS "${_candidate_header}")
    # header exists, set include dir
    get_filename_component(_nlohmann_include_dir "${_candidate_header}" DIRECTORY)
    message(STATUS "third_party: using vendored nlohmann/json headers at ${_nlohmann_include_dir}")
    if(NOT TARGET pylabhub::nlohmann_json)
      add_library(pylabhub::nlohmann_json INTERFACE)
      target_include_directories(pylabhub::nlohmann_json INTERFACE
        $<BUILD_INTERFACE:${_nlohmann_include_dir}>
        $<INSTALL_INTERFACE:include>
      )
      set(_nlohmann_target_created ON)
    endif()
  else()
    message(STATUS "third_party: vendored nlohmann/json requested but header not found at ${_candidate_header}")
  endif()
endif()

# 2) If not vendored (or vendored absent), try FetchContent
if(NOT _nlohmann_target_created)
  include(FetchContent)

  # If the user wanted to avoid network/fetching, they can set PREFER_VENDOR_NLOHMANN=ON
  message(STATUS "third_party: FetchContent'ing nlohmann/json (header-only)")
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
  )
  FetchContent_MakeAvailable(nlohmann_json)

  # FetchContent sets nlohmann_json_SOURCE_DIR / _BINARY_DIR automatically.
  if(TARGET nlohmann_json::nlohmann_json)
    add_library(pylabhub::nlohmann_json ALIAS nlohmann_json::nlohmann_json)
    message(STATUS "third_party: aliased pylabhub::nlohmann_json -> nlohmann_json::nlohmann_json")
    set(_nlohmann_target_created ON)
  else()
    # Create INTERFACE target from fetched source tree if possible
    if(DEFINED nlohmann_json_SOURCE_DIR)
      set(_maybe_include_dir "${nlohmann_json_SOURCE_DIR}/include")
      if(EXISTS "${_maybe_include_dir}/nlohmann/json.hpp" OR EXISTS "${_maybe_include_dir}/json.hpp")
        set(_nlohmann_include_dir "${_maybe_include_dir}")
        if(NOT TARGET pylabhub::nlohmann_json)
          add_library(pylabhub::nlohmann_json INTERFACE)
          target_include_directories(pylabhub::nlohmann_json INTERFACE
            $<BUILD_INTERFACE:${_nlohmann_include_dir}>
            $<INSTALL_INTERFACE:include>
          )
          message(STATUS "third_party: created pylabhub::nlohmann_json INTERFACE -> ${_nlohmann_include_dir}")
          set(_nlohmann_target_created ON)
        else()
          message(STATUS "third_party: pylabhub::nlohmann_json target already exists")
          message(STATUS "third_party: nlohmann_json fetched, but target creation skipped")
        endif()
      else()
        message(WARNING "third_party: fetched nlohmann_json but could not find headers under ${_maybe_include_dir}")
      endif()
    else()
      message(WARNING "third_party: FetchContent made available nlohmann_json but ${nlohmann_json_SOURCE_DIR} not set")
    endif()
  endif()
endif()

# 3) If still not created, leave a clear status
if(NOT _nlohmann_target_created)
  message(STATUS "third_party: nlohmann_json target not created (vendor absent and fetch fallback failed).")
endif()

# Alias an upstream-provided target if it exists
if(TARGET nlohmann_json::nlohmann_json AND NOT TARGET pylabhub::nlohmann_json)
  add_library(pylabhub::nlohmann_json ALIAS nlohmann_json::nlohmann_json)
endif()

# 4) Export a canonical name to the parent (only if created)
if(_nlohmann_target_created)
  set(NLOHMANN_TARGET "pylabhub::nlohmann_json" PARENT_SCOPE)
else()
  set(NLOHMANN_TARGET "" PARENT_SCOPE)
endif()

# 5) install vendored header 
if(THIRD_PARTY_INSTALL AND _nlohmann_target_created AND DEFINED _nlohmann_include_dir AND EXISTS "${_nlohmann_include_dir}")
  # install the main header or the include tree depending on layout
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
endif()
