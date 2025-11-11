# src/IgorXOP/CMakeLists.txt
# ------------------------------------------------------------------------------
# Build the pylabhub XOP plugin for Igor Pro (Windows x64 and macOS x64 only)
#
# Responsibilities:
#  - Detect XOPSupport via FindXOPSupport.cmake (find_xopsupport)
#  - Configure target sources & flags according to XOP Toolkit guidance
#    (Windows: Visual C++ settings; macOS: bundle/.xop packaging).
#  - Produce final artifact with .xop extension and correct internal executable name.
#  - Install only the final .xop and copy any .ipf/.iph/.ihf files from the src dir
#    into <install_prefix>/xop/.
#  - Publish the following CACHE INTERNAL variables for top-level summary:
#      BUILD_XOP_FINAL_ENABLED (ON/OFF)
#      XOP_TARGET_NAME         (string)
#      XOPTOOLKIT_DIR         (path used)
#      XOP_SUPPORT_FOUND      (ON/OFF)
#
# Notes:
#  - Top-level should define option(BUILD_XOP ...) and call add_subdirectory(src/IgorXOP)
#  - This file intentionally performs detection itself so the top-level need only
#    add the subdir when BUILD_XOP is ON.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)
project(IgorXOP LANGUAGES C CXX)

# Ensure local cmake helpers are visible
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Prefer top-level BUILD_XOP option, otherwise provide a local one
if(NOT DEFINED BUILD_XOP)
  option(BUILD_XOP "Attempt to build Igor XOP addon (only supported on Windows x64 and macOS)" ON)
endif()

# Initialize published cache variables (will be updated later)
set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
set(XOP_TARGET_NAME "" CACHE INTERNAL "Final: IgorXOP target name")
set(XOPTOOLKIT_DIR "" CACHE INTERNAL "XOPSupport root used by IgorXOP")
set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")

message(STATUS "==============================================")
message(STATUS "IgorXOP: BUILD_XOP = ${BUILD_XOP}")

if(NOT BUILD_XOP)
  message(STATUS "IgorXOP: BUILD_XOP=OFF; skipping XOP configuration.")
  message(STATUS "==============================================")
  return()
endif()

# Ensure find_xopsupport is available (define include path fallback)
if(NOT COMMAND find_xopsupport)
  include(cmake/FindXOPSupport.cmake OPTIONAL)
endif()

if(NOT COMMAND find_xopsupport)
  message(WARNING "IgorXOP: find_xopsupport() not available (FindXOPSupport.cmake not found). Skipping XOP.")
  # ensure cache variables reflect skip
  set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
  set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")
  message(STATUS "==============================================")
  return()
endif()

# Call the find helper. It sets imported targets (XOP::XOPSupport / XOP::IGOR) and publishes XOP_SUPPORT_ROOT_USED.
find_xopsupport(_xop_found)

if(NOT _xop_found)
  message(WARNING "IgorXOP: XOPSupport not found; skipping XOP configuration.")
  set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
  set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")
  if(DEFINED XOP_SUPPORT_ROOT_USED)
    set(XOPTOOLKIT_DIR "${XOP_SUPPORT_ROOT_USED}" CACHE INTERNAL "XOPSupport root used by IgorXOP")
  else()
    set(XOPTOOLKIT_DIR "" CACHE INTERNAL "XOPSupport root used by IgorXOP")
  endif()
  message(STATUS "==============================================")
  return()
endif()

# Publish detection result for top-level use
set(XOP_SUPPORT_FOUND ON CACHE INTERNAL "XOPSupport found (vendor or system)")
if(DEFINED XOP_SUPPORT_ROOT_USED)
  set(XOPTOOLKIT_DIR "${XOP_SUPPORT_ROOT_USED}" CACHE INTERNAL "XOPSupport root used by IgorXOP")
else()
  set(XOPTOOLKIT_DIR "" CACHE INTERNAL "XOPSupport root used by IgorXOP")
endif()

message(STATUS "IgorXOP: XOPSupport detected; XOPSupport root: ${XOPTOOLKIT_DIR}")
message(STATUS "IgorXOP: configuring XOP target (platform specific behavior follows; see XOPMan8-devtools).")

# -------------------------------------------------------------------------
# Platform support check: only WIN x64 and APPLE (Intel x64) are supported.
# -------------------------------------------------------------------------
if(DEFINED CMAKE_OSX_ARCHITECTURES)
  list(FIND CMAKE_OSX_ARCHITECTURES "x86_64" _found_osx_x64)
  if(_found_osx_x64 EQUAL -1)
    message(WARNING "IgorXOP: CMAKE_OSX_ARCHITECTURES does not include x86_64; build may fail for this xop.")
  endif()
endif()
if(NOT PLATFORM_WIN64 AND NOT (PLATFORM_APPLE AND _found_osx_x64))
  message(WARNING "IgorXOP: only Windows x64 and macOS (x64) are supported. Skipping.")
  set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
  message(STATUS "==============================================")
  return()
endif()

# -------------------------------------------------------------------------
# Source list (platform-specific)
#   - macOS: include WaveAccess.r (Rez resource) if present (XOP toolkit expects .r)
#   - Windows: include WaveAccess.rc and WaveAccessWinCustom.rc if present
# -------------------------------------------------------------------------
set(XOP_NAME pylabhub-xop)          # base name of product
set(XOP_TARGET ${XOP_NAME})

set(XOP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.h
  ${CMAKE_CURRENT_SOURCE_DIR}/resource.h
)

# macOS resource file (.r) - Rez should be used only on macOS per toolkit docs. :contentReference[oaicite:1]{index=1}
if(PLATFORM_APPLE)
  set(_rfile "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r")
  if(EXISTS "${_rfile}")
    list(APPEND XOP_SOURCES "${_rfile}")
    message(STATUS "IgorXOP: adding macOS resource ${_rfile}")
  endif()
endif()

# Windows resource file(s)
if(PLATFORM_WIN64)
  set(_rcfile "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.rc")
  set(_rcfile_custom "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccessWinCustom.rc")
  if(EXISTS "${_rcfile}")
    list(APPEND XOP_SOURCES "${_rcfile}")
    message(STATUS "IgorXOP: adding Windows resource ${_rcfile}")
  endif()
  if(EXISTS "${_rcfile_custom}")
    list(APPEND XOP_SOURCES "${_rcfile_custom}")
    message(STATUS "IgorXOP: adding Windows custom resource ${_rcfile_custom}")
  endif()
endif()

# -------------------------------------------------------------------------
# Create target
# -------------------------------------------------------------------------
# On macOS we want a bundle that is packaged as .xop; on Windows a DLL renamed to .xop.
# We'll create a shared target and set properties per-platform.
add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})
add_library(pylabhub::${XOP_TARGET} ALIAS ${XOP_TARGET})

# macOS: create a bundle so CMake can produce a package folder (Contents/...).
# We still set SUFFIX to .xop so the bundle name ends with .xop.
if(PLATFORM_APPLE)
  set_target_properties(${XOP_TARGET} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.pylabhub.xop.${XOP_NAME}"
    MACOSX_BUNDLE_BUNDLE_NAME "${XOP_NAME}"
  )
endif()

# Ensure the output file uses the .xop wrapper extension and no 'lib' prefix on macOS and on Windows.
# On Windows we also append 64 to the output name to follow the toolkit's examples for 64-bit XOPs. :contentReference[oaicite:2]{index=2}
if(PLATFORM_WIN64)
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""                         # no 'lib' prefix
    OUTPUT_NAME "${XOP_NAME}64"       # WaveAccess64 style naming for 64-bit
    SUFFIX ".xop"
  )
elseif(PLATFORM_APPLE)
  # For macOS bundle, set prefix "" and suffix .xop to make the package name end with .xop
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""
    SUFFIX ".xop"
  )
endif()

# -------------------------------------------------------------------------
# Include directories and linking (prefer imported target XOP::XOPSupport)
# -------------------------------------------------------------------------
if(TARGET XOP::XOPSupport)
  target_include_directories(${XOP_TARGET} PRIVATE
    $<TARGET_PROPERTY:XOP::XOPSupport,INTERFACE_INCLUDE_DIRECTORIES>
  )
  message(STATUS "IgorXOP: using include dirs from XOP::XOPSupport imported target")
elseif(DEFINED XOP_SUPPORT_ROOT_USED AND EXISTS "${XOP_SUPPORT_ROOT_USED}/include")
  target_include_directories(${XOP_TARGET} PRIVATE "${XOP_SUPPORT_ROOT_USED}/include")
  message(STATUS "IgorXOP: using include dir ${XOP_SUPPORT_ROOT_USED}/include")
elseif(DEFINED XOP_VENDOR_DIR AND EXISTS "${XOP_VENDOR_DIR}")
  # last-resort vendor dir
  target_include_directories(${XOP_TARGET} PRIVATE "${XOP_VENDOR_DIR}")
  message(STATUS "IgorXOP: using vendor include dir ${XOP_VENDOR_DIR}")
endif()

# Link to XOP support
if(TARGET XOP::XOPSupport)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::XOPSupport)
  message(STATUS "IgorXOP: linking to XOP::XOPSupport imported target")
else()
  if(DEFINED XOP_SUPPORT_LIB_PATH AND EXISTS "${XOP_SUPPORT_LIB_PATH}")
    target_link_libraries(${XOP_TARGET} PRIVATE "${XOP_SUPPORT_LIB_PATH}")
    message(STATUS "IgorXOP: linking directly to ${XOP_SUPPORT_LIB_PATH}")
  endif()
endif()

# Optional IGOR import lib
if(TARGET XOP::IGOR)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::IGOR)
  message(STATUS "IgorXOP: linking to XOP::IGOR imported target")
elseif(DEFINED XOP_SUPPORT_IGOR_LIB_PATH AND EXISTS "${XOP_SUPPORT_IGOR_LIB_PATH}")
  target_link_libraries(${XOP_TARGET} PRIVATE "${XOP_SUPPORT_IGOR_LIB_PATH}")
  message(STATUS "IgorXOP: linking to IGOR import lib ${XOP_SUPPORT_IGOR_LIB_PATH}")
endif()

# -------------------------------------------------------------------------
# Compiler flags and MSVC-specific runtime configuration (per XOP dev notes)
#  - Add _CRT_SECURE_NO_WARNINGS
#  - Add /utf-8 for MSVC
#  - Prefer static CRT (MultiThreaded / MultiThreadedDebug) per XOP guide. :contentReference[oaicite:3]{index=3}
# -------------------------------------------------------------------------
target_compile_features(${XOP_TARGET} PRIVATE cxx_std_17)

# --- Debug/Release configuration helpers (MSVC-aware) ---
#  - Define debug/release compile-time macros consistently:
#      Debug configs:   _DEBUG and DEBUG
#      Non-Debug:       NDEBUG
#  - Choose MSVC runtime library per-configuration:
#      Debug => /MTd (MultiThreadedDebug)
#      Non-Debug => /MT (MultiThreaded)
#  - Keep /utf-8 and _CRT_SECURE_NO_WARNINGS applied for MSVC.
#
if(MSVC)
  # Always suppress deprecation warnings for MSVC CRT functions
  target_compile_definitions(${XOP_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
  # Keep UTF-8 source interpretation in MSVC
  target_compile_options(${XOP_TARGET} PRIVATE /utf-8)
  # Per-configuration compile definitions:
  #   - For Debug configs (any config with name "Debug"), define _DEBUG and DEBUG
  #   - For non-Debug configs, define NDEBUG
  target_compile_definitions(${XOP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Debug>:DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
  )
  # CRT selection:
  #   - Use /MTd on Debug (static debug CRT)
  #   - Use /MT on non-Debug (static release CRT)
  # Use generator expressions so behavior works in multi-config IDEs.
  # For CMake < 3.15, uncomment the following and comment out the set_property() line.
  # target_compile_options(${XOP_TARGET} PRIVATE
  #   $<$<CONFIG:Debug>:/MTd>
  #   $<$<NOT:$<CONFIG:Debug>>:/MT>
  # )
  set_property(TARGET ${XOP_TARGET} PROPERTY
    MSVC_RUNTIME_LIBRARY "$<$<CONFIG:Debug>:MultiThreadedDebug>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>"
  )
else()
  # Non-MSVC (clang/gcc): set portable debug macros and leave optimization flags to top-level.
  # Define DEBUG for Debug configs and NDEBUG for non-Debug configs.
  target_compile_definitions(${XOP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
  )
endif()

if(MSVC)
  target_compile_definitions(${XOP_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(${XOP_TARGET} PRIVATE /utf-8)

  # Set MSVC runtime library per-configuration using the modern MSVC runtime property (CMake >= 3.15)
  # Use static CRT: MultiThreaded (Release) / MultiThreadedDebug (Debug)
  # If using older CMake, this will be ignored; you can also set compile options /MT manually per-config.
  set_property(TARGET ${XOP_TARGET} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )

  # The XOP dev notes recommend Multi-Threaded static runtime to avoid redistributable DLL issues. :contentReference[oaicite:4]{index=4}
endif()

# System libraries on Windows
if(PLATFORM_WIN64)
  target_link_libraries(${XOP_TARGET} PRIVATE version)  # version.lib required per toolkit docs. :contentReference[oaicite:5]{index=5}
endif()

# -------------------------------------------------------------------------
# Post-build helper: copy any packaging/help files with specific suffixes
# (only copy files with .ihf, .ipf, suffixes from the src dir).
# The name is not fixed, so we detect by suffix.
# -------------------------------------------------------------------------
# Search for files in source dir with the allowed suffixes (non-recursive)
file(GLOB XOP_IHF_FILES  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.ihf")
file(GLOB XOP_IPF_FILES  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.ipf")

# Combine lists
set(_xop_package_files "")
list(APPEND _xop_package_files ${XOP_IHF_FILES})
list(APPEND _xop_package_files ${XOP_IPF_FILES})

if(_xop_package_files)
  message(STATUS "IgorXOP: will copy the following packaging/help files next to built XOP:")
  foreach(_f IN LISTS _xop_package_files)
    message(STATUS "  - ${_f}")
  endforeach()

  # Add a post-build step for each file so it is copied next to the built artifact
  foreach(_f IN LISTS _xop_package_files)
    add_custom_command(TARGET ${XOP_TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${_f}"
        "$<TARGET_FILE_DIR:${XOP_TARGET}>/${_f}"
      COMMENT "Copying packaging/help file ${_f} next to built XOP"
    )
  endforeach()
else()
  message(VERBOSE "IgorXOP: no .ihf/.ipf files found in ${CMAKE_CURRENT_SOURCE_DIR} to copy post-build.")
endif()

# -------------------------------------------------------------------------
# Install rules (reuse package-file list produced for post-build)
#  - Install only the final .xop package into <install_prefix>/xop/
#  - Also copy any .ipf, .ihf files from the source dir into the same folder.
#  - Do NOT add the plugin to a third-party export set (this is a project subproject).
# -------------------------------------------------------------------------

# 1) Install the built XOP artifact into <install_root>/xop
install(TARGETS ${XOP_TARGET}
  RUNTIME DESTINATION xop     # Windows (DLL-like)
  BUNDLE DESTINATION xop      # macOS bundle
  LIBRARY DESTINATION xop     # other possible layout
  INCLUDES DESTINATION include
)

# 2) Install packaging/help files (.ipf, .ihf).
#    Prefer to reuse the list computed for the post-build copy step (_xop_package_files).
#    If that variable is not present (e.g. code reordered), compute it once here as fallback.

if(_xop_package_files)
  foreach(_rel IN LISTS _xop_package_files)
    # Source file path (absolute) so install() has correct source reference
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_rel}")
    if(EXISTS "${_src}")
      install(FILES "${_src}" DESTINATION xop)
      message(STATUS "IgorXOP: scheduling install of ${_rel} -> <install-root>/xop/")
    else()
      message(WARNING "IgorXOP: expected package file ${_src} not found at install scheduling time")
    endif()
  endforeach()
else()
  message(VERBOSE "IgorXOP: no .ipf/.iph/.ihf packaging files found in ${CMAKE_CURRENT_SOURCE_DIR}; nothing to install into xop/")
endif()

# -------------------------------------------------------------------------
# Publish final state and messages for top-level
# -------------------------------------------------------------------------
set(BUILD_XOP_FINAL_ENABLED ON CACHE INTERNAL "Final: IgorXOP will be built")
set(XOP_TARGET_NAME "${XOP_TARGET}" CACHE INTERNAL "Final: IgorXOP target name")
# XOPTOOLKIT_DIR already set above
message(STATUS "IgorXOP: configured XOP target '${XOP_TARGET}'.")
message(STATUS "  XOP build added:    ${XOP_TARGET}")
if(XOPTOOLKIT_DIR)
  message(STATUS "  XOPTOOLKIT used:   ${XOPTOOLKIT_DIR}")
else()
  message(STATUS "  XOPTOOLKIT used:   <unknown>")
endif()
message(STATUS "==============================================")
