# /cpp/tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.18)

include(GoogleTest)
include(StageHelpers)

add_subdirectory(helpers)

# Helper: create a test executable with common settings
function(pylabhub_add_test_executable name)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs SOURCES)
  cmake_parse_arguments(_args "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  add_executable(${name} ${_args_SOURCES})
  target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name}
    PRIVATE
      pylabhub::utils
      pylabhub::third_party::gtest
      pylabhub_test_helpers
  )
  target_compile_features(${name} PRIVATE cxx_std_20)

  if(MSVC)
    target_compile_options(${name} PRIVATE /EHsc /wd5105 /Zc:preprocessor)
  endif()
endfunction()

# Test executables
pylabhub_add_test_executable(utils_tests SOURCES
  test_atomicguard.cpp
  test_recursionguard.cpp
)

pylabhub_add_test_executable(multiprocess_tests SOURCES
  test_filelock.cpp
  test_jsonconfig.cpp
)

pylabhub_add_test_executable(test_logger SOURCES
  # if test_logger has sources, list them here; kept empty previously
)
# Now add the logger subdir which will append sources to test_logger
add_subdirectory(logger)

# Place test executables into the staged runtime directory so tests and DLLs are co-located.
set(STAGED_BIN_DIR "${PYLABHUB_STAGING_DIR}/bin")

set_target_properties(utils_tests multiprocess_tests test_logger PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${STAGED_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${STAGED_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${STAGED_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${STAGED_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${STAGED_BIN_DIR}"
)

# Ensure an aggregator 'stage_tests' target exists for staging macros to attach to
if (NOT TARGET stage_tests)
  add_custom_target(stage_tests COMMENT "Aggregate target for staging test executables")
endif()

# Ensure staged library is available before staging/running tests
if (TARGET stage_pylabhub_utils)
  add_dependencies(stage_tests stage_pylabhub_utils)
endif()

# Stage each test executable into the staging tree
pylabhub_stage_executable(TARGET utils_tests DESTINATION tests ATTACH_TO stage_tests)
pylabhub_stage_executable(TARGET multiprocess_tests DESTINATION tests ATTACH_TO stage_tests)
pylabhub_stage_executable(TARGET test_logger DESTINATION tests ATTACH_TO stage_tests)

# Make test targets depend on the staged library so building tests stages the lib first
add_dependencies(utils_tests stage_pylabhub_utils)
add_dependencies(multiprocess_tests stage_pylabhub_utils)
add_dependencies(test_logger stage_pylabhub_utils)

# Staging layout variables
set(STAGED_LIB_DIR "${PYLABHUB_STAGING_DIR}/bin")
set(STAGED_RUNTIME_DIR "${PYLABHUB_STAGING_DIR}/bin")  # runtime dir = staged bin
set(STAGED_TEST_DIR "${PYLABHUB_STAGING_DIR}/tests")

# Discover tests and apply per-discovered-test properties (working dir + PATH)
foreach(tgt IN ITEMS utils_tests multiprocess_tests test_logger)
  gtest_discover_tests(${tgt}
    WORKING_DIRECTORY "${STAGED_RUNTIME_DIR}"
    TEST_PROPERTIES
      "WORKING_DIRECTORY=${STAGED_RUNTIME_DIR}"
      "ENVIRONMENT=PATH=${STAGED_LIB_DIR};$ENV{PATH}"
  )
endforeach()

message(STATUS "Configured test executables: utils_tests, multiprocess_tests, test_logger")
