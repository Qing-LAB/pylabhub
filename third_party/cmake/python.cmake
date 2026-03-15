# third_party/cmake/python.cmake
#
# Python integration using astral-sh/python-build-standalone.
#
# Two modes:
#
# A) Developer build (default):
#    Part 1: Configure-time download + extraction (cached after first run).
#            Downloads the standalone tarball, extracts it into the build tree,
#            then points find_package(Python) at it so pybind11 can compile
#            against the exact 3.14 headers/libs.
#    Part 3: Run-time staging — copies standalone Python to opt/python.
#
# B) Wheel build (SKBUILD=ON, set by scikit-build-core):
#    Skips the standalone download. Uses the host Python provided by
#    scikit-build-core/cibuildwheel. The wheel does NOT bundle a Python
#    runtime — the user's own Python is used at runtime.
#
# Part 2 (pybind11 targets) is shared by both modes.

include(ThirdPartyPolicyAndHelper)
include(StageHelpers)

# ===========================================================================
# SKBUILD MODE — use host Python, skip standalone download
# ===========================================================================
if(SKBUILD)
  message(STATUS "[pylabhub-third-party] SKBUILD mode: using host Python (no standalone download)")

  # The executables use pybind11::embed, which requires Development.Embed
  # (libpythonX.Y.so). This is NOT available on all manylinux Python builds
  # (e.g., cp39 on manylinux_2_28). Those versions are skipped via CIBW_SKIP.
  find_package(Python REQUIRED COMPONENTS Interpreter Development.Module Development.Embed)
  message(STATUS "[pylabhub-third-party] Found Python ${Python_VERSION}: ${Python_EXECUTABLE}")

  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pybind11/CMakeLists.txt")
    message(FATAL_ERROR
      "[pylabhub-third-party] pybind11 submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/pybind11. "
      "Run: git submodule update --init third_party/pybind11")
  endif()
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/pybind11 EXCLUDE_FROM_ALL)

  add_library(pylabhub_pybind11_module INTERFACE)
  target_link_libraries(pylabhub_pybind11_module INTERFACE pybind11::module)
  add_library(pylabhub::third_party::pybind11_module ALIAS pylabhub_pybind11_module)

  add_library(pylabhub_pybind11_embed INTERFACE)
  target_link_libraries(pylabhub_pybind11_embed INTERFACE pybind11::embed)
  add_library(pylabhub::third_party::pybind11_embed ALIAS pylabhub_pybind11_embed)

  message(STATUS "[pylabhub-third-party] pybind11 targets created (wheel mode, no staging).")
  return()  # Skip Parts 0-3 (standalone download, staging, pip env)
endif()

# ===========================================================================
# PART 0: PLATFORM + VERSION (developer build only)
# ===========================================================================
message(STATUS "[pylabhub-third-party] Configuring Python build-time dependencies...")

set(PYTHON_STANDALONE_VERSION "3.14.3+20260211")
set(PYTHON_RELEASE_TAG        "20260211")
set(PYTHON_RELEASE_BASE_URL
    "https://github.com/astral-sh/python-build-standalone/releases/download/${PYTHON_RELEASE_TAG}")

if(PLATFORM_LINUX_X86_64)
  set(_py_filename "cpython-${PYTHON_STANDALONE_VERSION}-x86_64-unknown-linux-gnu-install_only.tar.gz")
  set(_py_sha256   "a3917eee21b61c9d8bfab22a773d1fe6945683dd40b5d5b263527af2550e3bbf")
  set(_py_sentinel_rel "bin/python3")
elseif(PLATFORM_WIN64_X86_64)
  set(_py_filename "cpython-${PYTHON_STANDALONE_VERSION}-x86_64-pc-windows-msvc-install_only.tar.gz")
  set(_py_sha256   "fcaae26be290da3c51fa14d0e89fe004b7858ed285038938b18e5682b7f7c592")
  set(_py_sentinel_rel "python.exe")
elseif(PLATFORM_APPLE_AARCH64)
  set(_py_filename "cpython-${PYTHON_STANDALONE_VERSION}-aarch64-apple-darwin-install_only.tar.gz")
  set(_py_sha256   "df38f57df6b1030375d854e01bf7d4080971a2946b029fe5e8e86ff70efa2216")
  set(_py_sentinel_rel "bin/python3")
elseif(PLATFORM_APPLE_X86_64)
  set(_py_filename "cpython-${PYTHON_STANDALONE_VERSION}-x86_64-apple-darwin-install_only.tar.gz")
  set(_py_sha256   "f858d7c53b479bafd84812da79db061a7401fedd448deb65e81549728e4568f3")
  set(_py_sentinel_rel "bin/python3")
else()
  message(WARNING "[pylabhub-third-party] No standalone Python build for this platform. Skipping Python integration.")
  return()
endif()

set(_py_url          "${PYTHON_RELEASE_BASE_URL}/${_py_filename}")
set(_py_download_dir "${CMAKE_BINARY_DIR}/third_party/python_downloads")
set(_py_extract_dir  "${CMAKE_BINARY_DIR}/third_party/python_runtime_src")
set(_py_root         "${_py_extract_dir}/python")   # astral tarballs always unpack to python/
set(_py_sentinel     "${_py_root}/${_py_sentinel_rel}")

# ===========================================================================
# PART 1: CONFIGURE-TIME DOWNLOAD + EXTRACTION (runs once, then cached)
# ===========================================================================
if(NOT EXISTS "${_py_sentinel}")
  file(MAKE_DIRECTORY "${_py_download_dir}" "${_py_extract_dir}")

  set(_py_archive "${_py_download_dir}/${_py_filename}")

  # --- Obtain the archive: local override or download ---
  if(PYLABHUB_PYTHON_LOCAL_ARCHIVE AND EXISTS "${PYLABHUB_PYTHON_LOCAL_ARCHIVE}")
    # Offline / air-gapped build: use the caller-supplied archive.
    message(STATUS "[pylabhub-third-party] Using local Python archive: ${PYLABHUB_PYTHON_LOCAL_ARCHIVE}")
    # Verify SHA256 before trusting the local file.
    file(SHA256 "${PYLABHUB_PYTHON_LOCAL_ARCHIVE}" _local_sha256)
    string(TOLOWER "${_local_sha256}"  _local_sha256_lower)
    string(TOLOWER "${_py_sha256}"     _expected_sha256_lower)
    if(NOT _local_sha256_lower STREQUAL _expected_sha256_lower)
      message(FATAL_ERROR
        "[pylabhub-third-party] SHA256 mismatch for local Python archive!\n"
        "  Expected : ${_expected_sha256_lower}\n"
        "  Got      : ${_local_sha256_lower}\n"
        "  File     : ${PYLABHUB_PYTHON_LOCAL_ARCHIVE}\n"
        "Ensure the archive matches platform '${CMAKE_SYSTEM_PROCESSOR}' and "
        "version '${PYTHON_STANDALONE_VERSION}'.")
    endif()
    file(COPY_FILE "${PYLABHUB_PYTHON_LOCAL_ARCHIVE}" "${_py_archive}")
  elseif(NOT EXISTS "${_py_archive}")
    # Online build: download from GitHub.
    message(STATUS "[pylabhub-third-party] Downloading Python ${PYTHON_STANDALONE_VERSION} (this may take a minute)...")
    file(DOWNLOAD
        "${_py_url}" "${_py_archive}"
        EXPECTED_HASH "SHA256=${_py_sha256}"
        STATUS _dl_status
    )
    list(GET _dl_status 0 _dl_rc)
    if(NOT _dl_rc EQUAL 0)
      list(GET _dl_status 1 _dl_msg)
      file(REMOVE "${_py_archive}")   # remove partial download
      message(FATAL_ERROR
        "[pylabhub-third-party] Python download failed: ${_dl_msg}\n"
        "Tip: download the archive manually and set:\n"
        "  -DPYLABHUB_PYTHON_LOCAL_ARCHIVE=/path/to/${_py_filename}")
    endif()
    message(STATUS "[pylabhub-third-party] Python download complete.")
  endif()

  message(STATUS "[pylabhub-third-party] Extracting Python standalone...")
  execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf "${_py_archive}"
      WORKING_DIRECTORY "${_py_extract_dir}"
      RESULT_VARIABLE _ex_rc
  )
  if(NOT _ex_rc EQUAL 0)
    message(FATAL_ERROR "[pylabhub-third-party] Failed to extract Python standalone archive.")
  endif()
  message(STATUS "[pylabhub-third-party] Python standalone ready at: ${_py_root}")
endif()

# ===========================================================================
# PART 2: BUILD-TIME CONFIGURATION (pybind11 + compile targets)
# ===========================================================================

# Point CMake's Python finder at our standalone installation.
# Use CACHE variables so they survive re-configures without re-downloading.
set(Python_ROOT_DIR  "${_py_root}"     CACHE PATH     "Standalone Python 3.14 root" FORCE)
set(Python_FIND_STRATEGY "LOCATION"   CACHE STRING   "" FORCE)
set(Python_FIND_VIRTUALENV "STANDARD"  CACHE STRING   "" FORCE)

find_package(Python 3.14 EXACT REQUIRED COMPONENTS Interpreter Development)
message(STATUS "[pylabhub-third-party] Found Python ${Python_VERSION}: ${Python_EXECUTABLE}")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pybind11/CMakeLists.txt")
  message(FATAL_ERROR
    "[pylabhub-third-party] pybind11 submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/pybind11. "
    "Run: git submodule update --init third_party/pybind11")
endif()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/pybind11 EXCLUDE_FROM_ALL)

add_library(pylabhub_pybind11_module INTERFACE)
target_link_libraries(pylabhub_pybind11_module INTERFACE pybind11::module)
add_library(pylabhub::third_party::pybind11_module ALIAS pylabhub_pybind11_module)
message(STATUS "[pylabhub-third-party] Created pylabhub::third_party::pybind11_module target.")

# pybind11::embed — for embedding Python in a C++ executable.
# Provides Py_Initialize/Finalize, py::exec, py::scoped_interpreter, etc.
# pybind11::embed already transitively links the correct Python libraries;
# Python::Development.Embed is not added explicitly here to avoid CMake 3.29+
# compatibility issues with the legacy Python::Development target alias.
add_library(pylabhub_pybind11_embed INTERFACE)
target_link_libraries(pylabhub_pybind11_embed INTERFACE pybind11::embed)
add_library(pylabhub::third_party::pybind11_embed ALIAS pylabhub_pybind11_embed)
message(STATUS "[pylabhub-third-party] Created pylabhub::third_party::pybind11_embed target.")

# ===========================================================================
# PART 3: RUN-TIME STAGING (copies already-extracted standalone to stage/)
# ===========================================================================
if(NOT THIRD_PARTY_INSTALL)
  message(STATUS "[pylabhub-third-party] Skipping Python run-time staging (THIRD_PARTY_INSTALL is OFF).")
  return()
endif()

message(STATUS "[pylabhub-third-party] Configuring Python run-time staging...")

# The standalone is already downloaded at configure time; staging is a fast copy.
add_custom_target(stage_python_runtime
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${_py_root}"
        "${PYLABHUB_STAGING_DIR}/opt/python"
    COMMENT "Staging Python ${PYTHON_STANDALONE_VERSION} to ${PYLABHUB_STAGING_DIR}/opt/python"
    VERBATIM
)
add_dependencies(stage_third_party_deps stage_python_runtime)
message(STATUS "[pylabhub-third-party] Python will be staged to '${PYLABHUB_STAGING_DIR}/opt/python'.")

# ===========================================================================
# PART 3b: WINDOWS — stage Python DLLs to bin/ and tests/
#
# On Windows there is no RPATH.  The OS DLL search order checks the executable's
# own directory before PATH, so python3.dll and python3XX.dll (e.g. python314.dll)
# must live next to every .exe that embeds Python.  This mirrors the convention
# used for other third-party DLLs (libzmq, libsodium, …) in StageHelpers.cmake.
#
# The extension modules (.pyd files) and stdlib do NOT need to be in bin/ — they
# are resolved via PYTHONHOME which we set in python_interpreter.cpp.
# ===========================================================================
if(PLATFORM_WIN64)
  # Derive DLL name from the Python version set by find_package(Python).
  # Python 3.14 → python314.dll
  set(_py_major_minor "${Python_VERSION_MAJOR}${Python_VERSION_MINOR}")
  set(_py_dll         "${_py_root}/python${_py_major_minor}.dll")
  set(_py_stable_dll  "${_py_root}/python3.dll")

  add_custom_target(stage_python_dlls_win
      # bin/ — for the hubshell executable
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_py_stable_dll}" "${PYLABHUB_STAGING_DIR}/bin/python3.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_py_dll}"        "${PYLABHUB_STAGING_DIR}/bin/python${_py_major_minor}.dll"
      # tests/ — for embedded-Python test binaries
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_py_stable_dll}" "${PYLABHUB_STAGING_DIR}/tests/python3.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_py_dll}"        "${PYLABHUB_STAGING_DIR}/tests/python${_py_major_minor}.dll"
      DEPENDS stage_python_runtime
      COMMENT "Staging Python DLLs (python3.dll, python${_py_major_minor}.dll) to bin/ and tests/"
      VERBATIM
  )
  add_dependencies(stage_third_party_deps stage_python_dlls_win)
  message(STATUS "[pylabhub-third-party] Windows: Python DLLs will be staged to bin/ and tests/.")
endif()

# ===========================================================================
# PART 4: PREPARE_PYTHON_ENV — pip install requirements into staged Python
# ===========================================================================
if(NOT PYLABHUB_PREPARE_PYTHON_ENV)
  message(STATUS "[pylabhub-third-party] Skipping Python env preparation (PYLABHUB_PREPARE_PYTHON_ENV is OFF).")
  return()
endif()

set(_requirements_src "${PYLABHUB_PYTHON_REQUIREMENTS_FILE}")
if(NOT EXISTS "${_requirements_src}")
  message(FATAL_ERROR
    "[pylabhub-third-party] PYLABHUB_PYTHON_REQUIREMENTS_FILE not found: '${_requirements_src}'\n"
    "  Set -DPYLABHUB_PYTHON_REQUIREMENTS_FILE=<path> to a valid requirements file.")
endif()
message(STATUS "[pylabhub-third-party] pip requirements: ${_requirements_src}")
set(_requirements_dst "${PYLABHUB_STAGING_DIR}/share/scripts/python/requirements.txt")
set(_pip_stamp        "${PYLABHUB_STAGING_DIR}/opt/python/.pip_env_ready")

# Platform-specific path to the staged Python interpreter.
if(PLATFORM_WIN64)
  set(_staged_python "${PYLABHUB_STAGING_DIR}/opt/python/python.exe")
else()
  set(_staged_python "${PYLABHUB_STAGING_DIR}/opt/python/bin/python3")
endif()

# Build up pip install flags.
set(_pip_flags "")
if(PYLABHUB_PYTHON_WHEELS_DIR AND NOT PYLABHUB_PYTHON_WHEELS_DIR STREQUAL "")
  list(APPEND _pip_flags "--find-links" "${PYLABHUB_PYTHON_WHEELS_DIR}" "--no-index")
  message(STATUS "[pylabhub-third-party] pip will install from wheels dir: ${PYLABHUB_PYTHON_WHEELS_DIR}")
else()
  message(STATUS "[pylabhub-third-party] pip will install from PyPI (online).")
endif()

# Stage the requirements.txt from source to the staged scripts directory.
# This is an OUTPUT-based command so CMake tracks it as a real file dependency.
add_custom_command(
    OUTPUT  "${_requirements_dst}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${PYLABHUB_STAGING_DIR}/share/scripts/python"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_requirements_src}" "${_requirements_dst}"
    DEPENDS "${_requirements_src}"
    COMMENT "Staging requirements.txt"
    VERBATIM
)

# prepare_python_env: pip install into staged Python.
# The stamp file makes this idempotent — only re-runs when requirements.txt changes.
add_custom_command(
    OUTPUT  "${_pip_stamp}"
    COMMAND "${_staged_python}" -m pip install --progress-bar off --upgrade pip setuptools wheel ${_pip_flags}
    COMMAND "${_staged_python}" -m pip install --progress-bar off -r "${_requirements_dst}" ${_pip_flags}
    COMMAND ${CMAKE_COMMAND} -E touch "${_pip_stamp}"
    DEPENDS "${_requirements_dst}"
    COMMENT "pip: installing packages from requirements.txt into staged Python"
    VERBATIM
)
add_custom_target(prepare_python_env DEPENDS "${_pip_stamp}")
add_dependencies(prepare_python_env stage_python_runtime)

# Wire into the overall staging flow so 'stage_all' includes the pip step.
add_dependencies(stage_third_party_deps prepare_python_env)

message(STATUS "[pylabhub-third-party] prepare_python_env target configured (stamp: ${_pip_stamp}).")
