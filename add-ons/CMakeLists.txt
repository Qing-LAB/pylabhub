# /add-ons/CMakeLists.txt
#
# This file can be used to include subdirectories for add-on applications,
# tools, or plugins that use the project's core libraries.
if(BUILD_XOP)
  if(PLATFORM_WIN64_X86_64 OR PLATFORM_APPLE_X86_64)
    add_subdirectory(IgorXOP)
  else()
    message(STATUS "Skipping IgorXOP add-on: The current platform/architecture ('${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}') is not supported. IgorXOP requires Windows x64 or macOS x64.")
  endif()
else()
  message(STATUS "Skipping IgorXOP add-on (BUILD_XOP is OFF).")
endif()

