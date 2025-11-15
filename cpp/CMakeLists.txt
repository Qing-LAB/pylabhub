cmake_minimum_required(VERSION 3.18)

project(pylabhub-shell VERSION 0.1 LANGUAGES C CXX)

# Optionally force clang on macOS
option(FORCE_USE_CLANG_ON_APPLE "Force clang on macOS hosts" ON)
if(FORCE_USE_CLANG_ON_APPLE AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  find_program(_clang_path clang)
  find_program(_clangpp_path clang++)
  if(_clang_path AND _clangpp_path)
    set(CMAKE_C_COMPILER "${_clang_path}" CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${_clangpp_path}" CACHE FILEPATH "C++ compiler" FORCE)
    message(STATUS "Top-level: forcing clang/clang++ on macOS: ${_clang_path}, ${_clangpp_path}")
  else()
    message(WARNING "Top-level: clang not found on PATH. Consider installing Xcode or CommandLineTools.")
  endif()
endif()

# CMake helpers
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(cmake/PlatformAndCompiler.cmake)
include(cmake/ToplevelOptions.cmake)
include(cmake/StageHelpers.cmake)

find_package(Threads REQUIRED)

message(STATUS "")
message(STATUS "===============================================================")
message(STATUS "Processing third-party submodules in ${CMAKE_CURRENT_SOURCE_DIR}/third_party")

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
if(EXISTS "${THIRD_PARTY_DIR}/CMakeLists.txt")
  add_subdirectory("${THIRD_PARTY_DIR}" EXCLUDE_FROM_ALL)
else()
  message(STATUS "third_party/CMakeLists.txt not found; vendor includes may still be used.")
endif()

# Source discovery
file(GLOB_RECURSE UTIL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp")
file(GLOB_RECURSE FILEUTIL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/fileutil/*.cpp")
if(NOT UTIL_SOURCES) 
  message(WARNING "No sources found in src/util — check your source path.") 
endif()
if(NOT FILEUTIL_SOURCES) 
  message(WARNING "No sources found in src/fileutil — check your source path.") 
endif()

# corelib
set(PYLABHUB_CORELIB_SOURCES ${UTIL_SOURCES} ${FILEUTIL_SOURCES})
if(PYLABHUB_BUILD_SHARED)
  add_library(pylabhub-corelib SHARED ${PYLABHUB_CORELIB_SOURCES})
else()
  add_library(pylabhub-corelib STATIC ${PYLABHUB_CORELIB_SOURCES})
endif()
add_library(pylabhub::corelib ALIAS pylabhub-corelib)
message(STATUS "Created alias target pylabhub::corelib -> pylabhub-corelib")

target_include_directories(pylabhub-corelib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(NLOHMANN_VENDORED)
  target_include_directories(pylabhub-corelib
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/include>
      $<INSTALL_INTERFACE:include>
  )
endif()

# Link known third-party targets (if present)
if(TARGET pylabhub::nlohmann_json)
  message(STATUS "Linking pylabhub-corelib -> pylabhub::nlohmann_json")
  target_link_libraries(pylabhub-corelib PUBLIC pylabhub::nlohmann_json)
endif()
if(TARGET pylabhub::fmt)
  message(STATUS "Linking pylabhub-corelib -> pylabhub::fmt")
  target_link_libraries(pylabhub-corelib PRIVATE pylabhub::fmt)
endif()
if(TARGET pylabhub::zmq)
  message(STATUS "Linking pylabhub-corelib -> pylabhub::zmq")
  target_link_libraries(pylabhub-corelib PRIVATE pylabhub::zmq)
endif()

target_link_libraries(pylabhub-corelib PUBLIC Threads::Threads)
target_compile_features(pylabhub-corelib PUBLIC cxx_std_17)
set_target_properties(pylabhub-corelib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(PYLABHUB_BUILD_SHARED AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set_target_properties(pylabhub-corelib PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# Sanitizers unchanged
if(ENABLE_SANITIZERS)
  if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
    message(WARNING "ENABLE_SANITIZERS requested but compiler not supported. Disabling sanitizers.")
    set(ENABLE_SANITIZERS OFF CACHE BOOL "Enable sanitizers" FORCE)
  endif()
  if(APPLE)
    message(WARNING "Sanitizers requested but running on macOS. Disabling sanitizers by default on Apple platforms.")
    set(ENABLE_SANITIZERS OFF CACHE BOOL "Enable sanitizers" FORCE)
  endif()
endif()
if(ENABLE_SANITIZERS)
  target_compile_options(pylabhub-corelib PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(pylabhub-corelib PUBLIC -fsanitize=address,undefined)
endif()

# main executable
add_executable(pylabhub-shell src/coreshell.cpp)
target_link_libraries(pylabhub-shell PRIVATE pylabhub-corelib)

# IgorXOP subproject (left to subproject to stage XOP)
if(BUILD_XOP)
  add_subdirectory(src/IgorXOP)
  if(DEFINED XOP_TARGET_NAME AND NOT "${XOP_TARGET_NAME}" STREQUAL "")
    if(TARGET "${XOP_TARGET_NAME}" AND TARGET pylabhub::corelib)
      target_link_libraries("${XOP_TARGET_NAME}" PRIVATE pylabhub::corelib)
      message(STATUS "Top-level: linked XOP target ${XOP_TARGET_NAME} -> pylabhub::corelib (fallback)")
    endif()
  endif()
else()
  set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
  set(XOP_TARGET_NAME "" CACHE INTERNAL "Final: IgorXOP target name")
  set(XOPTOOLKIT_DIR "" CACHE INTERNAL "XOPSupport root used by IgorXOP")
  set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")
  message(STATUS "Skipping src/IgorXOP (BUILD_XOP=OFF)")
endif()

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# -------------------------------------------------------------------------
# Post-build staging tree
# -------------------------------------------------------------------------
set(_stage_root "${CMAKE_BINARY_DIR}/install")
set(_stage_bin "${_stage_root}/bin")
set(_stage_lib "${_stage_root}/lib")
set(_stage_include "${_stage_root}/include")
set(_stage_xop "${_stage_root}/xop")

add_custom_target(stage_install COMMENT "Stage built artifacts into ${_stage_root}")
add_custom_command(TARGET stage_install PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage_bin}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage_lib}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage_include}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage_xop}"
)

# project staging copies
add_custom_command(TARGET stage_install POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:pylabhub-shell> "${_stage_bin}/"
  COMMENT "Staging pylabhub-shell -> ${_stage_bin}"
)
add_custom_command(TARGET stage_install POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:pylabhub-corelib> "${_stage_lib}/"
  COMMENT "Staging pylabhub-corelib -> ${_stage_lib}"
)
add_custom_command(TARGET stage_install POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/include" "${_stage_include}"
  COMMENT "Staging project headers -> ${_stage_include}"
)

# XOP bundle dependency (if subproject exposes bundle target)
if(DEFINED XOP_TARGET_NAME AND NOT "${XOP_TARGET_NAME}" STREQUAL "")
  set(_maybe_bundle_target "${XOP_TARGET_NAME}_bundle")
  if(TARGET "${_maybe_bundle_target}")
    add_dependencies(stage_install "${_maybe_bundle_target}")
    message(STATUS "Top-level: stage_install depends on subproject bundle target ${_maybe_bundle_target} (subproject handles XOP staging).")
  else()
    message(STATUS "Top-level: XOP bundle target ${_maybe_bundle_target} not found; assuming subproject handles staging or assembly was skipped.")
  endif()
else()
  message(STATUS "Top-level: XOP subproject not configured or XOP_TARGET_NAME not set; no XOP staging dependency added.")
endif()

# make stage part of regular build
add_dependencies(stage_install pylabhub-shell pylabhub-corelib)
set_target_properties(stage_install PROPERTIES EXCLUDE_FROM_ALL FALSE)

# -------------------------------------------------------------------------
# Third-party staging
# -------------------------------------------------------------------------
set(_THIRD_PARTY_TARGETS_TO_STAGE
  pylabhub::fmt
  pylabhub::zmq
  pylabhub::nlohmann_json
)

message(STATUS "Top-level: attempting to stage third-party targets: ${_THIRD_PARTY_TARGETS_TO_STAGE}")

# IMPORTANT: call the helper with the *variable name* (no quotes) so it can expand the list:
message(STATUS "**** include ******")
stage_collect_and_schedule_includes(_THIRD_PARTY_TARGETS_TO_STAGE "${CMAKE_SOURCE_DIR}/third_party/include" "${_stage_include}" TARGET_STAGE stage_install)
message(STATUS "**** libs ******")
stage_schedule_third_party_libs(_THIRD_PARTY_TARGETS_TO_STAGE "${_stage_lib}" TARGET_STAGE stage_install)
stage_schedule_lib_duplicate_check("${_stage_lib}" TARGET_STAGE stage_install)

# Print resolved include roots (debugging aid)
if(DEFINED _STAGEHELPERS_RESOLVED_INCLUDES)
  message(STATUS "Top-level: StageHelpers resolved include roots: ${_STAGEHELPERS_RESOLVED_INCLUDES}")
endif()

# -------------------------------------------------------------------------
# Install: copy staged tree into <dest>/pyLabHub when user runs cmake --install
# -------------------------------------------------------------------------
install(CODE
"if(NOT EXISTS \"${CMAKE_BINARY_DIR}/install\")
  message(FATAL_ERROR \"pyLabHub install: staging directory '${CMAKE_BINARY_DIR}/install' not found. Run the staging build: cmake --build <build-dir> --target stage_install before running 'cmake --install'.\")
endif()
set(_dest_root \"${CMAKE_INSTALL_PREFIX}/pyLabHub\")
execute_process(COMMAND \"${CMAKE_COMMAND}\" -E remove_directory \"${_dest_root}\")
execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_directory \"${CMAKE_BINARY_DIR}/install\" \"${_dest_root}\"
  RESULT_VARIABLE _copy_res OUTPUT_VARIABLE _copy_out ERROR_VARIABLE _copy_err)
if(NOT _copy_res EQUAL 0)
  message(FATAL_ERROR \"pyLabHub install: failed to copy staging tree to '${_dest_root}': ${_copy_err}\")
endif()
file(WRITE \"${_dest_root}/pyLabHubEnv.sh\" \"# Source this file to set up pyLabHub environment variables\nexport PYLABHUB_HOME=\\\"${CMAKE_INSTALL_PREFIX}/pyLabHub\\\"\nexport PATH=\\\"\\$PYLABHUB_HOME/bin:\\$PATH\\\"\n\")
file(WRITE \"${_dest_root}/pyLabHubEnv.csh\" \"# Source this file to set up pyLabHub environment variables (csh/tcsh)\nsetenv PYLABHUB_HOME \\\"${CMAKE_INSTALL_PREFIX}/pyLabHub\\\"\nsetenv PATH \\\"\\$PYLABHUB_HOME/bin:\\$PATH\\\"\n\")
message(STATUS \"pyLabHub installed into ${CMAKE_INSTALL_PREFIX}/pyLabHub. To activate, run: source ${CMAKE_INSTALL_PREFIX}/pyLabHub/pyLabHubEnv.sh\")"
)

# Summary
message(STATUS "==============================================")
message(STATUS "CMake summary:")
if(DEFINED _platform_name) 
  message(STATUS "  Platform detected:  ${_platform_name}") 
endif()
message(STATUS "  Project:            ${PROJECT_NAME} (${PROJECT_VERSION})")
message(STATUS "  CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER}")
message(STATUS "  Build type:         ${CMAKE_BUILD_TYPE}")
message(STATUS "  Local stage root:   ${_stage_root}")
if(BUILD_XOP_FINAL_ENABLED)
  message(STATUS "  XOP build added:    ${XOP_TARGET_NAME}")
  message(STATUS "  XOPTOOLKIT used:    ${XOPTOOLKIT_DIR}")
endif()
message(STATUS "==============================================")
