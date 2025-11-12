cmake_minimum_required(VERSION 3.15)
project(pylabhub-tests NONE)

# ---------------------------------------------------------------------------
# Requirements
# ---------------------------------------------------------------------------
# Expect parent project to provide pylabhub-corelib targets.
# If those targets are not present, this file will still attempt to build tests,
# but linking will fail — that's intentional to make missing dependencies obvious.

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Compile-time log level for tests
# - numeric mapping: 0=TRACE,1=DEBUG,2=INFO,3=WARNING,4=ERROR,5=NONE
# - default: verbose in Debug builds, ERROR-only in Release builds
# You may override this from the top-level cmake command:
#   cmake -DLOGGER_COMPILE_LEVEL=0 .. 
# ---------------------------------------------------------------------------
if(NOT DEFINED LOGGER_COMPILE_LEVEL)
  if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(LOGGER_COMPILE_LEVEL 0 CACHE STRING "Compile-time log level for tests (0=TRACE...4=ERROR)")
  else()
    set(LOGGER_COMPILE_LEVEL 4 CACHE STRING "Compile-time log level for tests (0=TRACE...4=ERROR)")
  endif()
else()
  set(LOGGER_COMPILE_LEVEL ${LOGGER_COMPILE_LEVEL} CACHE STRING "Compile-time log level for tests (0=TRACE...4=ERROR)" FORCE)
endif()

# ---------------------------------------------------------------------------
# Test sources
# ---------------------------------------------------------------------------
set(TEST_SOURCES
    test_atomicguard.cpp
    test_logger.cpp
    test_filelock.cpp
    test_jsonconfig.cpp
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
    #   pylabhub-corelib       (contains utils, fileutil code)
    #
    # If your top-level CMake uses different target names, adjust these accordingly.
    if(TARGET pylabhub::pylabhub::nlohmann_json)
        target_link_libraries(${test_name} PRIVATE pylabhub::pylabhub::nlohmann_json)
    else()
        message(WARNING "Target 'pylabhub::pylabhub::nlohmann_json' not found. Ensure the top-level CMake defines it.")
    endif()

    if(TARGET pylabhub-corelib)
        target_link_libraries(${test_name} PRIVATE pylabhub-corelib)
    else()
        message(WARNING "Target 'pylabhub-corelib' not found. Ensure the top-level CMake defines it.")
    endif()
    # Make sure tests link fmt if pylabhub-core hides it (PRIVATE) ---
    # If pylabhub-corelib did not expose fmt via PUBLIC, tests still need fmt available because
    # Logger headers include <fmt/format.h>. Detect common fmt targets and link them.
    if(TARGET fmt::fmt)
        target_link_libraries(${test_name} PRIVATE fmt::fmt)
    elseif(TARGET fmt::fmt-header-only)
        # header-only variant: define FMT_HEADER_ONLY so fmt behaves as header-only if needed
        target_compile_definitions(${test_name} PRIVATE FMT_HEADER_ONLY=1)
        target_link_libraries(${test_name} PRIVATE fmt::fmt-header-only)
    else()
        # If fmt isn't a CMake target (e.g. not added as submodule), try to find it with find_package
        find_package(fmt QUIET CONFIG)
        if(TARGET fmt::fmt)
            target_link_libraries(${test_name} PRIVATE fmt::fmt)
        else()
            message(WARNING "fmt target not found for test '${test_name}'. If pylabhub-corelib links fmt PRIVATE, tests will fail to compile. Add fmt as a submodule or install it system-wide.")
        endif()
    endif()

    # Link threads
    target_link_libraries(${test_name} PRIVATE Threads::Threads)

    # Ensure C++17
    target_compile_features(${test_name} PRIVATE cxx_std_17)

    # Pass compile-time log level to the test target
    target_compile_definitions(${test_name} PRIVATE LOGGER_COMPILE_LEVEL=${LOGGER_COMPILE_LEVEL})

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
message(STATUS "Tests CMake: LOGGER_COMPILE_LEVEL=${LOGGER_COMPILE_LEVEL}")
