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

set(_all_test_targets "")
foreach(src IN LISTS TEST_SOURCES)
    get_filename_component(test_name ${src} NAME_WE)
    add_executable(${test_name} ${src})
    add_executable(pylabhub::${test_name} ALIAS ${test_name})
    list(APPEND _all_test_targets ${test_name})

    # Link against the core library alias. This is the ONLY dependency needed.
    # pylabhub::corelib will provide all other necessary targets (like fmt, zmq, Threads)
    # and include directories through its PUBLIC/INTERFACE properties.
    if(TARGET pylabhub::corelib)
        target_link_libraries(${test_name} PRIVATE pylabhub::corelib)
    else()
        message(FATAL_ERROR "Target 'pylabhub::corelib' not found. The test suite requires the main project to be configured first.")
    endif()

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
# Staging for Test Executables
#
# Following the project's design, staging for tests is handled by a dedicated
# custom target that is only triggered by the global `stage_all` target.
# This is only active if the user intends to create an installable package.
# ---------------------------------------------------------------------------
if(PYLABHUB_CREATE_INSTALL_TARGET)
  set(_staging_commands "")
  foreach(test_name IN LISTS _all_test_targets)
    # For each test, add a sequence of commands to the list.
    list(APPEND _staging_commands
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${test_name}>" "${PYLABHUB_STAGING_DIR}/tests/"
        COMMAND ${CMAKE_COMMAND}
            -D TARGET_NAME=${test_name}
            -D DEST_DIR=${PYLABHUB_STAGING_DIR}/tests
            -D "RUNTIME_DEPS=$<TARGET_RUNTIME_DLLS:${test_name}>"
            -P "${CMAKE_SOURCE_DIR}/cmake/StageRuntimeDeps.cmake"
        COMMENT "Staging test '${test_name}'"
    )
  endforeach()

  # Create the single custom target with all the commands.
  add_custom_target(stage_tests
      # The first command creates the directory once.
      COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/tests"
      # Then, execute all the copy commands for each test.
      ${_staging_commands}
      COMMENT "Staging all test executables and their dependencies"
      VERBATIM
  )

  # Ensure tests are built before staging and that `stage_all` triggers test staging.
  add_dependencies(stage_tests ${_all_test_targets})
  add_dependencies(stage_all stage_tests)
  message(STATUS "Tests: Configured 'stage_tests' target for on-demand staging.")
endif()


# ---------------------------------------------------------------------------
# Extra: friendly summary
# ---------------------------------------------------------------------------
message(STATUS "Tests CMake: LOGGER_COMPILE_LEVEL=${LOGGER_COMPILE_LEVEL}")
