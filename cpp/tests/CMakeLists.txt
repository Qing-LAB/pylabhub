cmake_minimum_required(VERSION 3.15)
project(pylabhub-tests NONE)

# ---------------------------------------------------------------------------
# Requirements
# ---------------------------------------------------------------------------
# Expect parent project to provide pylabhub-util and pylabhub-fileutil targets.
# If those targets are not present, this file will still attempt to build tests,
# but linking will fail — that's intentional to make missing dependencies obvious.

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Compile-time log level for tests
# - numeric mapping: 0=TRACE,1=DEBUG,2=INFO,3=WARNING,4=ERROR,5=NONE
# - default: verbose in Debug builds, ERROR-only in Release builds
# You may override this from the top-level cmake command:
#   cmake -DLOG_COMPILE_LEVEL=0 .. 
# ---------------------------------------------------------------------------
if(NOT DEFINED LOG_COMPILE_LEVEL)
  if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(LOG_COMPILE_LEVEL 0 CACHE STRING "Compile-time log level for tests (0=TRACE...4=ERROR)")
  else()
    set(LOG_COMPILE_LEVEL 4 CACHE STRING "Compile-time log level for tests (0=TRACE...4=ERROR)")
  endif()
else()
  set(LOG_COMPILE_LEVEL ${LOG_COMPILE_LEVEL} CACHE STRING "Compile-time log level for tests (0=TRACE...4=ERROR)" FORCE)
endif()

# ---------------------------------------------------------------------------
# Test sources
# ---------------------------------------------------------------------------
set(TEST_SOURCES
    test_atomicguard.cpp
    test_filelock.cpp
    test_jsonconfig.cpp
    test_logger.cpp
)

# ---------------------------------------------------------------------------
# Create test executables and register with CTest
# ---------------------------------------------------------------------------
enable_testing()

foreach(src IN LISTS TEST_SOURCES)
    get_filename_component(test_name ${src} NAME_WE)
    add_executable(${test_name} ${src})

    # Make sure test can find project headers
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

    # Link with project libraries. Expect the root CMake to have defined these targets:
    #   pylabhub-util       (contains logger, utils)
    #   pylabhub-fileutil   (fileutil code)
    #
    # If your top-level CMake uses different target names, adjust these accordingly.
    if(TARGET pylabhub)
        target_link_libraries(${test_name} PRIVATE pylabhub)
    else()
        message(WARNING "Target 'pylabhub' not found. Ensure the top-level CMake defines it.")
    endif()

    # Link threads
    target_link_libraries(${test_name} PRIVATE Threads::Threads)

    # Ensure C++17
    target_compile_features(${test_name} PRIVATE cxx_std_17)

    # Pass compile-time log level to the test target
    target_compile_definitions(${test_name} PRIVATE LOG_COMPILE_LEVEL=${LOG_COMPILE_LEVEL})

    # Compiler-specific options
    if(MSVC)
        target_compile_options(${test_name} PRIVATE /W3 /permissive-)
    else()
        target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()

    # Add to CTest
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ---------------------------------------------------------------------------
# Extra: friendly summary
# ---------------------------------------------------------------------------
message(STATUS "Tests CMake: LOG_COMPILE_LEVEL=${LOG_COMPILE_LEVEL}")
