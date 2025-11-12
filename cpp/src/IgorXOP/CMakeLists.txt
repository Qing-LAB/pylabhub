# src/IgorXOP/CMakeLists.txt
# ------------------------------------------------------------------------------
# Build the pylabhub XOP plugin for Igor Pro (Windows x64 and macOS x64 only)
#
# Responsibilities:
#  - Detect XOPSupport via FindXOPSupport.cmake (find_xopsupport)
#  - Configure target sources & flags according to XOP Toolkit guidance
#  - Produce final artifact with .xop extension and correct internal executable name
#  - Install the final .xop and copy any Igor procedure/help files (.ipf/.ihf)
#  - Publish CACHE INTERNAL variables for top-level summary:
#      BUILD_XOP_FINAL_ENABLED, XOP_TARGET_NAME, XOPTOOLKIT_DIR, XOP_SUPPORT_FOUND
#
# Assumptions (per your instruction):
#  - PLATFORM_WIN64 and PLATFORM_APPLE are correctly set by higher-level CMake.
#  - Only Windows x64 and macOS x64 are supported for this XOP.
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

# Ensure find_xopsupport is available (include fallback)
if(NOT COMMAND find_xopsupport)
  include(cmake/FindXOPSupport.cmake OPTIONAL)
endif()

if(NOT COMMAND find_xopsupport)
  message(WARNING "IgorXOP: find_xopsupport() not available (FindXOPSupport.cmake not found). Skipping XOP.")
  set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
  set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")
  message(STATUS "==============================================")
  return()
endif()

# Call find helper. It may create imported targets XOP::XOPSupport and XOP::IGOR
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
message(STATUS "IgorXOP: configuring XOP target (platform specific behavior follows).")

# -------------------------------------------------------------------------
# Platform check (use PLATFORM_* values supplied by top-level)
# - We assume PLATFORM_WIN64 and PLATFORM_APPLE are set by top-level.
# - Additionally, for macOS we must verify x86_64 (Intel) target; do robust detection.
# -------------------------------------------------------------------------
if(NOT (DEFINED PLATFORM_WIN64 OR DEFINED PLATFORM_APPLE))
  message(WARNING "IgorXOP: top-level did not provide PLATFORM_WIN64 or PLATFORM_APPLE. Skipping.")
  set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
  message(STATUS "==============================================")
  return()
endif()

# Detect macOS x86_64 target robustly:
#  - If CMAKE_OSX_ARCHITECTURES defined, check it for x86_64
#  - Else check CMAKE_HOST_SYSTEM_PROCESSOR / CMAKE_SYSTEM_PROCESSOR
#  - Else, fall back to pointer-size heuristic (64-bit implies x86_64 on macOS host), with warning.
set(_macos_x86_64 OFF)
if(DEFINED PLATFORM_APPLE)
  if(DEFINED CMAKE_OSX_ARCHITECTURES)
    list(FIND CMAKE_OSX_ARCHITECTURES "x86_64" _idx)
    if(NOT _idx EQUAL -1)
      set(_macos_x86_64 ON)
    endif()
  endif()

  if(NOT _macos_x86_64)
    if(DEFINED CMAKE_HOST_SYSTEM_PROCESSOR)
      string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _proc_lower)
      if(_proc_lower MATCHES "x86_64" OR _proc_lower MATCHES "amd64")
        set(_macos_x86_64 ON)
      endif()
    endif()
  endif()

  if(NOT _macos_x86_64 AND DEFINED CMAKE_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sysproc_lower)
    if(_sysproc_lower MATCHES "x86_64" OR _sysproc_lower MATCHES "amd64")
      set(_macos_x86_64 ON)
    endif()
  endif()

  if(NOT _macos_x86_64)
    # Last-resort heuristic: pointer size 8 on macOS commonly indicates 64-bit target.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      message(WARNING "IgorXOP: macOS target pointer size is 64-bit; assuming x86_64. If building universal or arm64, verify CMAKE_OSX_ARCHITECTURES.")
      set(_macos_x86_64 ON)
    else()
      message(WARNING "IgorXOP: macOS target does not appear to be x86_64. Skipping XOP (we only support macOS x64).")
      set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
      message(STATUS "==============================================")
      return()
    endif()
  endif()
endif()

# At this point, PLATFORM_* and macOS x64 detection passed (if applicable)
if(DEFINED PLATFORM_WIN64)
  message(STATUS "IgorXOP: target platform = Windows x64 (PLATFORM_WIN64=ON)")
endif()
if(DEFINED PLATFORM_APPLE)
  message(STATUS "IgorXOP: target platform = macOS (PLATFORM_APPLE=ON); x86_64 = ${_macos_x86_64}")
endif()

# -------------------------------------------------------------------------
# Source list (platform-specific)
# -------------------------------------------------------------------------
set(XOP_NAME pylabhub-xop)          # base product name
set(XOP_TARGET ${XOP_NAME})

set(XOP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.h
  ${CMAKE_CURRENT_SOURCE_DIR}/resource.h
)

# macOS resource file (.r)
if(DEFINED PLATFORM_APPLE)
  set(_rfile "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r")
  if(EXISTS "${_rfile}")
    list(APPEND XOP_SOURCES "${_rfile}")
    message(STATUS "IgorXOP: adding macOS resource ${_rfile}")
  endif()
endif()

# Windows resource file(s)
if(DEFINED PLATFORM_WIN64)
  set(_rcfile "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.rc")
  # The WaveAccessWinCustom.rc is included by WaveAccess.rc so don't added it separately.
  # set(_rcfile_custom "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccessWinCustom.rc")
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
add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})
add_library(pylabhub::${XOP_TARGET} ALIAS ${XOP_TARGET})

# macOS: create bundle (Contents/...) and ensure .xop suffix
if(DEFINED PLATFORM_APPLE)
  # Ensure the bundle and executable names are consistent and include the "64" suffix
  set_target_properties(${XOP_TARGET} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "pylabhub.xop.${XOP_NAME}"
    # IMPORTANT: make the bundle name *match* the executable and the toolkit convention (XOP64)
    MACOSX_BUNDLE_BUNDLE_NAME "${XOP_NAME}64"
    # Explicit executable name inside Contents/MacOS/
    MACOSX_BUNDLE_EXECUTABLE "${XOP_NAME}64"
    OUTPUT_NAME "${XOP_NAME}64"
    PREFIX ""              # no lib prefix
    SUFFIX ".xop"          # bundle ends with .xop
  )
endif()

# Windows output naming (no lib prefix, output name ends with 64 and suffix .xop)
if(DEFINED PLATFORM_WIN64)
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""                         # no lib prefix
    OUTPUT_NAME "${XOP_NAME}64"       # toolkit convention for 64-bit
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
  target_include_directories(${XOP_TARGET} PRIVATE "${XOP_VENDOR_DIR}")
  message(STATUS "IgorXOP: using vendor include dir ${XOP_VENDOR_DIR}")
endif()

# Link to XOP support library / imported target
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

if(DEFINED PLATFORM_APPLE)
  # macOS frameworks required by libXOPSupport64.a
  # Cocoa (AppKit + Foundation), CoreFoundation, CoreServices, Carbon, AudioToolbox, and Security if needed.
  target_link_libraries(${XOP_TARGET} PRIVATE
    "-framework Cocoa"          # AppKit + Foundation (UI classes like NSAlert, NSOpenPanel)
    "-framework CoreFoundation" # CFString, CFURL, CFRelease, etc.
    "-framework CoreServices"   # FS* / other legacy APIs
    "-framework Carbon"         # some old Carbon APIs used by XOPSupport
    "-framework AudioToolbox"   # AudioServicesPlayAlertSound
  )
endif()

# -------------------------------------------------------------------------
# Compiler flags and Debug/Release handling
# - Consolidated MSVC handling (no duplicate blocks)
# - Use generator expressions so multi-config generators work correctly
# -------------------------------------------------------------------------
# Set C++ standard requirement
target_compile_features(${XOP_TARGET} PRIVATE cxx_std_17)

if(MSVC)
  # Common MSVC flags
  target_compile_definitions(${XOP_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(${XOP_TARGET} PRIVATE /utf-8)

  # Debug vs Release compile macros
  target_compile_definitions(${XOP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Debug>:DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
  )

  # Select static CRT per-config (prefer static CRT as recommended by XOP docs)
  target_compile_options(${XOP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:/MTd>
    $<$<NOT:$<CONFIG:Debug>>:/MT>
  )
else()
  # Non-MSVC compilers: set portable debug macros; let optimization flags be controlled globally
  target_compile_definitions(${XOP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
  )
  # Optional: set -g/-O2 here if you want to force them for this target.
endif()

# Windows system libs
if(DEFINED PLATFORM_WIN64)
  target_link_libraries(${XOP_TARGET} PRIVATE version)  # version.lib per toolkit docs
endif()

# -------------------------------------------------------------------------
# Define XOP-standard macros required by XOPStandardHeaders.h
# Per XOPStandardHeaders.h: define MACIGOR/IGOR64 on macOS x64; WINIGOR/IGOR64 on Win64.
# See XOPStandardHeaders.h for details. :contentReference[oaicite:1]{index=1}
# -------------------------------------------------------------------------
if(DEFINED PLATFORM_APPLE)
  # We only build for macOS x86_64 (above we ensured _macos_x86_64)
  if(_macos_x86_64)
    target_compile_definitions(${XOP_TARGET} PRIVATE MACIGOR IGOR64)
    message(STATUS "IgorXOP: adding compile definitions for macOS: MACIGOR IGOR64")
  else()
    message(WARNING "IgorXOP: PLATFORM_APPLE set but macOS x86_64 detection failed; not adding MACIGOR/IGOR64")
  endif()
endif()

# --- macOS blocks handling: enable -fblocks for Clang; handle GCC cautiously ---
if(DEFINED PLATFORM_APPLE)
  include(CheckCXXCompilerFlag)

  # Test whether the current C++ compiler accepts -fblocks
  check_cxx_compiler_flag("-fblocks" CXX_HAS_FBLOCKS)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
    if(CXX_HAS_FBLOCKS)
      message(STATUS "IgorXOP: enabling -fblocks for Clang/AppleClang so Apple headers with '^' compile")
      target_compile_options(${XOP_TARGET} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fblocks>
      )
    else()
      message(WARNING "IgorXOP: Clang on this host does not accept -fblocks (unexpected). System headers using '^' may fail.")
    endif()

  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC on macOS is uncommon. Try best-effort fallback, but warn strongly.
    if(CXX_HAS_FBLOCKS)
      message(WARNING "IgorXOP: GCC detected on macOS and -fblocks is supported. This is uncommon; proceed with caution.")
      # Safer: compile the specific TU(s) that include macOS frameworks as Objective-C++.
      # This avoids C++ parser issues and allows blocks/ObjC++ constructs.
      set(_wa_cpp "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.cpp")
      if(EXISTS "${_wa_cpp}")
        message(STATUS "IgorXOP: compiling ${_wa_cpp} as Objective-C++ (OBJCXX) and enabling -fblocks")
        set_source_files_properties(${_wa_cpp} PROPERTIES LANGUAGE OBJCXX)
        target_compile_options(${XOP_TARGET} PRIVATE
          $<$<COMPILE_LANGUAGE:OBJCXX>:-fblocks>
        )
      else()
        message(WARNING "IgorXOP: WaveAccess.cpp not found; cannot set OBJCXX fallback.")
      endif()
    else()
      message(WARNING "IgorXOP: GCC detected and -fblocks not supported. Building may fail when system headers with '^' are included. Recommend using AppleClang.")
    endif()

  else()
    message(WARNING "IgorXOP: Unrecognized compiler '${CMAKE_CXX_COMPILER_ID}' on macOS. If you see '^' token errors, try building with AppleClang.")
  endif()
endif()

if(DEFINED PLATFORM_WIN64)
  target_compile_definitions(${XOP_TARGET} PRIVATE WINIGOR IGOR64)
  message(STATUS "IgorXOP: adding compile definitions for Windows: WINIGOR IGOR64")
endif()

# -------------------------------------------------------------------------
# Setup linker and Info.plist options for macOS to export XOPMain symbol
# - Per XOP documentation, XOPMain must be exported from the Mach-O binary.
# - This is done via an exports file (Exports.exp) passed to the linker.
# -------------------------------------------------------------------------
if(DEFINED PLATFORM_APPLE)
  message(STATUS "IgorXOP: configuring macOS linker options to export XOPMain symbol")
  # ensure Exports.exp is passed to the linker so XOPMain is exported from the Mach-O
  set(_exports_file "${CMAKE_CURRENT_SOURCE_DIR}/Exports.exp")
  if(EXISTS "${_exports_file}")
    target_link_options(${XOP_TARGET} PRIVATE
      $<$<CONFIG:Debug>:-Wl,-exported_symbols_list,${_exports_file}>
      $<$<NOT:$<CONFIG:Debug>>:-Wl,-exported_symbols_list,${_exports_file}>
    )
    message(STATUS "IgorXOP: using exported symbols file ${_exports_file}")
  else()
    message(WARNING "IgorXOP: Exports.exp not found at ${_exports_file}; XOPMain may not be exported.")
  endif()
  # Configure Info.plist for the macOS bundle
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Info64.plist.in"
                "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
                @ONLY)
  set_target_properties(${XOP_TARGET} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
  )

endif()
# -------------------------------------------------------------------------
# Packaging/helper file discovery (discover once, reuse for post-build and install)
# - Allowed suffixes: .ihf, .ipf, (non-recursive in source dir)
# -------------------------------------------------------------------------
file(GLOB _xop_ihf_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.ihf")
file(GLOB _xop_ipf_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.ipf")

set(_xop_package_files "")
list(APPEND _xop_package_files ${_xop_ihf_files})
list(APPEND _xop_package_files ${_xop_ipf_files})

if(_xop_package_files)
  message(STATUS "IgorXOP: will copy the following packaging/help files next to built XOP:")
  foreach(_f IN LISTS _xop_package_files)
    message(STATUS "  - ${_f}")
  endforeach()

  foreach(_f IN LISTS _xop_package_files)
    add_custom_command(TARGET ${XOP_TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${_f}"
        "$<TARGET_FILE_DIR:${XOP_TARGET}>/${_f}"
      COMMENT "Copying Igor procedure/help file ${_f} next to built XOP"
    )
  endforeach()
else()
  message(VERBOSE "IgorXOP: no .ihf/.ipf files found in ${CMAKE_CURRENT_SOURCE_DIR} to copy post-build.")
endif()

# -------------------------------------------------------------------------
# Install rules
#  - Install only the final .xop package into <install_prefix>/xop/
#  - Also copy any discovered .ihf/.ipf  files into that same folder.
#  - Do NOT export this target in the third-party export set (this is a subproject artifact).
# -------------------------------------------------------------------------
install(TARGETS ${XOP_TARGET}
  RUNTIME DESTINATION xop     # Windows-like runtime
  BUNDLE DESTINATION xop      # macOS bundle
  LIBRARY DESTINATION xop     # other possible layouts
  INCLUDES DESTINATION include
)

if(DEFINED _xop_package_files AND _xop_package_files)
  foreach(_rel IN LISTS _xop_package_files)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_rel}")
    if(EXISTS "${_src}")
      install(FILES "${_src}" DESTINATION xop)
      message(STATUS "IgorXOP: scheduling install of ${_rel} -> <install-root>/xop/")
    else()
      message(WARNING "IgorXOP: expected package file ${_src} not found at install scheduling time")
    endif()
  endforeach()
else()
  message(VERBOSE "IgorXOP: no packaging/help files to install into xop/")
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
