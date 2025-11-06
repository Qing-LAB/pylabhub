# src/IgorXOP/CMakeLists.txt
# Build pylabhub-xop XOP module
# NOTE: ensure this file is added via add_subdirectory(...) from top-level CMakeLists.txt

# no cmake_minimum_required here (use top-level)

set(XOP_NAME pylabhub-xop)
set(XOP_TARGET ${XOP_NAME})

# === sources: update this list to match your real sources ===
set(XOP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.h
  ${CMAKE_CURRENT_SOURCE_DIR}/resource.h
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccessWinCustom.rc
  # add your other .c/.cpp/.rc/.r files here
)

# Create the shared library target first
add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})

# On Windows we want an XOP (DLL renamed to .xop) and a consistent output name:
if(WIN32)
  # produce e.g. pylabhub-xop64.xop
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""                                     # no "lib" prefix
    OUTPUT_NAME "${XOP_NAME}64"                   # name without extension
    SUFFIX ".xop"                                 # produce .xop instead of .dll
  )
endif()

# === compile options / defs ===
target_compile_features(${XOP_TARGET} PRIVATE cxx_std_17) # or cxx_std_14 if you need

# prefer static CRT (vendor uses /MT)
if(MSVC)
  target_compile_definitions(${XOP_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(${XOP_TARGET} PRIVATE /utf-8)
  message(STATUS "Setting MSVC runtime library to ${CMAKE_MSVC_RUNTIME_LIBRARY} for target ${XOP_TARGET}")
endif()

# === include directories: prefer INTERFACE_INCLUDE_DIRECTORIES from imported target if present ===
if(TARGET XOP::XOPSupport)
  # The imported target should already advertise include dirs; also make them visible for this target
  target_include_directories(${XOP_TARGET} PRIVATE $<TARGET_PROPERTY:XOP::XOPSupport,INTERFACE_INCLUDE_DIRECTORIES>)
else()
  # fallback: allow using top-level XOP_VENDOR_DIR or system includes
  if(DEFINED XOP_VENDOR_DIR AND EXISTS "${XOP_VENDOR_DIR}")
    target_include_directories(${XOP_TARGET} PRIVATE "${XOP_VENDOR_DIR}")
  endif()
endif()

# === link vendor libs (must be done AFTER add_library) ===
# Link the XOPSupport imported target (resolves XOPSupport symbols)
if(TARGET XOP::XOPSupport)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::XOPSupport)
  message(STATUS "Linking ${XOP_TARGET} -> XOP::XOPSupport")
else()
  message(WARNING "XOP::XOPSupport target not found; ensure third_party/XOPToolkit/XOPSupport is available or set -DUSE_SYSTEM_XOP=ON")
endif()

# Link IGOR import library if available (resolves MemError/NewPtr/GetHandleSize/... symbols)
if(TARGET XOP::IGOR)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::IGOR)
  message(STATUS "Linking ${XOP_TARGET} -> XOP::IGOR")
else()
  message(WARNING "XOP::IGOR target not found; IGOR import lib may be required for linking on Windows.")
endif()

# Link in any additional system libs the XOPSupport/IGOR expect (example)
if(WIN32)
  target_link_libraries(${XOP_TARGET} PRIVATE version) # add other Windows libs if needed
endif()

# === post-build: copy help/resources next to the output (optional) ===
set(XOP_HELP "${CMAKE_CURRENT_SOURCE_DIR}/${XOP_NAME} Help.ihf") # adjust filename if different
if(EXISTS "${XOP_HELP}")
  add_custom_command(TARGET ${XOP_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${XOP_HELP}"
      "$<TARGET_FILE_DIR:${XOP_TARGET}>/${XOP_NAME} Help.ihf"
    COMMENT "Copying XOP help file next to built xop"
  )
endif()

# === install rule for convenience ===
install(TARGETS ${XOP_TARGET}
  RUNTIME DESTINATION dist   # for windows/dll/.xop
  BUNDLE DESTINATION dist    # for macOS bundle
  LIBRARY DESTINATION dist
)

message(STATUS "Configured XOP target '${XOP_TARGET}'. Build with your chosen generator (e.g. Visual Studio x64 or Xcode).")
