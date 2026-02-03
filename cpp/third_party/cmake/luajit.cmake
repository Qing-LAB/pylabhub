# third_party/cmake/luajit.cmake
# Wrapper to build LuaJIT using ExternalProject (Makefile-based).
# Keeps an out-of-source build by copying sources to the binary dir,
# then runs make / make install with sanitized CFLAGS.

include(ExternalProject)
include(ThirdPartyPolicyAndHelper)

# Option to manually specify LuaJIT RELVER if git detection fails
set(THIRD_PARTY_LUAJIT_RELVER "" CACHE STRING "Manually specify LuaJIT RELVER (e.g., 2.1.0-beta3) if git detection is unavailable or fails.")

if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/luajit")
set(_build_dir "${CMAKE_BINARY_DIR}/third_party/luajit-build")
set(_install_dir "${PREREQ_INSTALL_DIR}")

# Sanitize flags (strip -march etc.)
pylabhub_sanitize_compiler_flags("CMAKE_C_FLAGS" _clean_c_flags)
pylabhub_sanitize_compiler_flags("CMAKE_CXX_FLAGS" _clean_cxx_flags)

# Choose make program (prefer CMake's detection)
if(DEFINED CMAKE_MAKE_PROGRAM AND CMAKE_MAKE_PROGRAM)
  set(_make_prog "${CMAKE_MAKE_PROGRAM}")
else()
  find_program(_found_make make)
  if(_found_make)
    set(_make_prog "${_found_make}")
  else()
    set(_make_prog "make")
  endif()
endif()

# parallel args if specified via CMake
if(DEFINED CMAKE_BUILD_PARALLEL_LEVEL AND CMAKE_BUILD_PARALLEL_LEVEL)
  set(_par_args "-j${CMAKE_BUILD_PARALLEL_LEVEL}")
else()
  set(_par_args "")
endif()

# Determine LuaJIT's RELVER dynamically from git if available
set(_luajit_relver_arg "")
find_program(_git_executable git)
if(_git_executable AND EXISTS "${_source_dir}/.git")
  # Run git describe in the source directory to get the version
  execute_process(
    COMMAND "${_git_executable}" describe --tags --always
    WORKING_DIRECTORY "${_source_dir}"
    OUTPUT_VARIABLE _git_desc_output
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_desc_result
  )
  if(_git_desc_result EQUAL 0 AND _git_desc_output)
    set(_luajit_detected_version "${_git_desc_output}")
    message(STATUS "[pylabhub-third-party][luajit] Detected version from git: ${_luajit_detected_version}")
  else()
    # Git found, but describe failed or returned empty. Check user option.
    if(THIRD_PARTY_LUAJIT_RELVER)
      set(_luajit_detected_version "${THIRD_PARTY_LUAJIT_RELVER}")
      message(STATUS "[pylabhub-third-party][luajit] Git detection failed; using user-provided version: ${THIRD_PARTY_LUAJIT_RELVER}")
    else()
      message(FATAL_ERROR "[pylabhub-third-party][luajit] Failed to determine version from git in ${_source_dir}. "
                         "Ensure the git submodule is initialized and up-to-date, and that a tag is reachable. "
                         "Alternatively, set the THIRD_PARTY_LUAJIT_RELVER CMake option.")
    endif()
  endif()
else()
  # Git not found or not a git repository. Check user option.
  if(THIRD_PARTY_LUAJIT_RELVER)
    set(_luajit_detected_version "${THIRD_PARTY_LUAJIT_RELVER}")
    message(STATUS "[pylabhub-third-party][luajit] Git not available; using user-provided version: ${THIRD_PARTY_LUAJIT_RELVER}")
  else()
    message(FATAL_ERROR "[pylabhub-third-party][luajit] Git command not found or LuaJIT source directory (${_source_dir}) is not a git repository. "
                       "Cannot determine dynamic version. Please ensure git is in PATH and the submodule is initialized, "
                       "or set the THIRD_PARTY_LUAJIT_RELVER CMake option.")
  endif()
endif()

# Ensure staging directories exist
file(MAKE_DIRECTORY "${_install_dir}")
file(MAKE_DIRECTORY "${_install_dir}/lib")
file(MAKE_DIRECTORY "${_install_dir}/include")
file(MAKE_DIRECTORY "${_install_dir}/include/luajit")
file(MAKE_DIRECTORY "${_build_dir}")

# Create ExternalProject that:
#  - copies sources into build dir
#  - calls make in that dir
#  - calls make install with PREFIX set to the staging dir
ExternalProject_Add(luajit_external
  SOURCE_DIR "${_source_dir}"
  BINARY_DIR "${_build_dir}"
  DOWNLOAD_COMMAND ""

  # Copy sources to the build dir (keeps original tree untouched).
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E copy_directory "${_source_dir}" "${_build_dir}"

  # Generate luajit_relver.txt in the build directory for LuaJIT's Makefile.
  COMMAND ${CMAKE_COMMAND} -E echo "${_luajit_detected_version}" > "${_build_dir}/luajit_relver.txt"

  # Build: run make in the build dir. Use cmake -E env to set CC/CFLAGS without shell quoting.
  BUILD_COMMAND
    ${CMAKE_COMMAND} -E env
      "CC=${CMAKE_C_COMPILER}"
      "CFLAGS=${_clean_c_flags}"
    ${_make_prog} -C "${_build_dir}" ${_par_args}

  # Install: pass PREFIX=<install_dir> as a separate token to avoid shell interpretation issues.
  # Using make install with PREFIX is what LuaJIT's Makefile supports.
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E env
      "CC=${CMAKE_C_COMPILER}"
      "CFLAGS=${_clean_c_flags}"
    ${_make_prog} -C "${_build_dir}" install PREFIX=${_install_dir}

  # Run detect script after install
  COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake"

  # Conservative byproduct: static lib or shared lib depending on build variant.
  BUILD_BYPRODUCTS
    "${_install_dir}/lib/luajit-stable.a"
)

# Write detect script for LuaJIT (used by your detect template or standalone)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "set(PACKAGE_NAME \"luajit\")\n")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "set(PREREQ_INSTALL_DIR \"${_install_dir}\")\n")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "set(PACKAGE_BINARY_DIR \"${_build_dir}\")\n")
# Patterns: include both libluajit/luajit and platform libs
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake"
  "set(LIB_PATTERNS \"libluajit*.a;luajit*.a;libluajit*.so;luajit*.so;libluajit*.dylib;luajit*.dylib\")\n")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake"
  "set(HEADER_SOURCE_PATTERNS \"include/luajit-*\")\n")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "set(STABLE_BASENAME \"luajit-stable\")\n")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "include(\"${CMAKE_CURRENT_LIST_DIR}/detect_external_project.cmake.in\")\n")

# Imported target: point to staged install dir; detect script will adjust exact filename if needed.
if(NOT TARGET pylabhub::third_party::luajit)
  add_library(pylabhub::third_party::luajit UNKNOWN IMPORTED GLOBAL)
  set_target_properties(pylabhub::third_party::luajit PROPERTIES
    IMPORTED_LOCATION "${_install_dir}/lib/luajit-stable.a"
    INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${_install_dir}/include/luajit>;$<INSTALL_INTERFACE:include/luajit>"
  )
endif()

add_dependencies(pylabhub::third_party::luajit luajit_external)

if(NOT TARGET luajit::pylabhub)
  add_library(luajit::pylabhub ALIAS pylabhub::third_party::luajit)
endif()

# Staging hooks (if your helper provides these functions)
if(DEFINED THIRD_PARTY_INSTALL AND THIRD_PARTY_INSTALL)
  if(COMMAND pylabhub_register_library_for_staging)
    pylabhub_register_library_for_staging(TARGET pylabhub::third_party::luajit)
  endif()
  if(COMMAND pylabhub_register_headers_for_staging)
      pylabhub_register_headers_for_staging(
        DIRECTORIES "${PREREQ_INSTALL_DIR}/include/luajit"
        SUBDIR "luajit"
        EXTERNAL_PROJECT_DEPENDENCY luajit_external
      )  
  endif()
  message(STATUS "[pylabhub-third-party] Staging luajit headers and library.")
else()
  message(STATUS "[pylabhub-third-party] THIRD_PARTY_INSTALL is OFF; skipping staging for luajit.")
endif()
