# src/IgorXOP/CMakeLists.txt
#
# Builds the pylabhub-xop XOP from sources in this directory.
cmake_minimum_required(VERSION 3.15)

set(XOP_NAME pylabhub-xop)            # package/base name (no arch suffix)
set(XOP_TARGET ${XOP_NAME})          # CMake target name

# Source files (update to match your project)
set(XOP_SOURCES
  src/xop/main.cpp        # replace with your actual XOP source files
  src/xop/XOPResources.c
  # add other .c/.cpp files here
)

# Resource/help files that should be placed beside the resulting .xop
set(XOP_HELP_FILES
  src/xop/pylabhub-xop Help.ihf   # example, update actual help filename(s)
)

# Include vendor include dir if available
if(TARGET XOP::XOPSupport)
  message(STATUS "Using vendor XOPSupport include via target XOP::XOPSupport")
  add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::XOPSupport)
  target_include_directories(${XOP_TARGET} PRIVATE $<TARGET_PROPERTY:XOP::XOPSupport,INTERFACE_INCLUDE_DIRECTORIES>)
else()
  # fallback: assume headers available via system paths
  add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})
  if(NOT USE_SYSTEM_XOP)
    message(FATAL_ERROR "XOPSupport not found and -DUSE_SYSTEM_XOP=OFF. Place XOPSupport under third_party/XOPToolkit/XOPSupport or enable -DUSE_SYSTEM_XOP")
  endif()
endif()

# Compiler options recommended by manual
if(MSVC)
  target_compile_definitions(${XOP_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(${XOP_TARGET} PRIVATE /utf-8)
endif()

# Per-platform packaging rules
if(APPLE)
  # On macOS, XOP is a bundle folder with .xop extension.
  set_target_properties(${XOP_TARGET} PROPERTIES
    FRAMEWORK FALSE
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "${XOP_NAME}64"           # e.g. pylabhub-xop64
    BUNDLE_EXTENSION "xop"
  )

  # Ensure exported symbol XOPMain is exported (manual uses Exports.exp in Xcode; mimic here by linker flags)
  # If you need to export XOPMain explicitly, use a .exp or -exported_symbols_list on mac. Example below:
  set(EXPORTS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Exports.exp")
  if(EXISTS "${EXPORTS_FILE}")
    # Use -exported_symbols_list for Apple clang
    target_link_options(${XOP_TARGET} PRIVATE "-Wl,-exported_symbols_list,${EXPORTS_FILE}")
  endif()

  # Info.plist - expect Info64.plist in the src/xop folder; copy into bundle
  set(INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info64.plist")
  if(EXISTS "${INFO_PLIST}")
    # Tell CMake to use this plist as the bundle Info
    set_target_properties(${XOP_TARGET} PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${INFO_PLIST}")
  endif()

  # Additional link to static XOPSupport if target not used (or target linked above will include lib)
  # Ensure proper library search path if vendor lib is used
  if(EXISTS "${XOP_VENDOR_DIR}/Xcode/libXOPSupport64.a")
    # already handled by imported target if present
  endif()

  # After build: ensure bundle contains help file(s) next to executable
  add_custom_command(TARGET ${XOP_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Packaging macOS .xop bundle..."
    COMMAND ${CMAKE_COMMAND} -E sleep 0
    # copy help files / compiled .ihf into bundle Contents/Resources or top-level as manual suggests (same folder)
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_SOURCE_DIR}/src/xop/${XOP_NAME}\\ Help.ihf
      $<TARGET_FILE_DIR:${XOP_TARGET}>/../
    COMMENT "Post-build: copying help/resources (if present) into XOP bundle directory."
  )

elseif(WIN32)
  # On Windows, the XOP is just a DLL with .xop extension
  # Adjust target properties to produce .xop filename
  set_target_properties(${XOP_TARGET} PROPERTIES
    OUTPUT_NAME "${XOP_NAME}64"   # e.g. pylabhub-xop64
    PREFIX ""                     # no lib prefix
    SUFFIX ".xop"
  )

  # set output directory into build tree root (similar to Visual C++ practice in manual)
  set_target_properties(${XOP_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/src/xop/VC"
  )

  # Additional linker inputs recommended by manual
  target_link_libraries(${XOP_TARGET} PRIVATE version)  # version.lib

  # copy help files next to .xop
  add_custom_command(TARGET ${XOP_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Copying help files next to ${XOP_NAME}64.xop..."
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/src/xop/${XOP_NAME} Help.ihf"
      "$<TARGET_FILE_DIR:${XOP_TARGET}>/$<TARGET_FILE_NAME:${XOP_TARGET}>"
    COMMENT "Post-build: copying help file."
  )
else()
  # Linux/other: treat like shared object; user may adapt
  set_target_properties(${XOP_TARGET} PROPERTIES
    OUTPUT_NAME "${XOP_NAME}64"
  )
endif()

# Provide convenient alias if desired
add_library(${XOP_NAME} ALIAS ${XOP_TARGET})

# Install rule: place produced .xop into a "dist" folder for convenience
install(TARGETS ${XOP_TARGET}
  RUNTIME DESTINATION dist
  BUNDLE DESTINATION dist
  LIBRARY DESTINATION dist
)

message(STATUS "Configured XOP target: ${XOP_TARGET}. Build with `cmake --build .` and find the .xop in the build output.")
