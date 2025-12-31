# tests/helpers/CMakeLists.txt

# This library contains common helper code for all test executables,
# including the main() entry point, worker dispatch logic, and worker implementations.
file(GLOB_RECURSE pylabhub_test_worker_srcs 
  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
  "*.cpp"
)

add_library(pylabhub_test_helpers_and_workers 
  STATIC
  ${pylabhub_test_worker_srcs}
)

if(MSVC)
  # Enable the standards-conforming preprocessor
  target_compile_options(pylabhub_test_helpers_and_workers PRIVATE /Zc:preprocessor)
endif()

target_compile_features(pylabhub_test_helpers_and_workers PRIVATE cxx_std_20)
target_link_libraries(pylabhub_test_helpers_and_workers
  PRIVATE
    pylabhub::basic
    pylabhub::third_party::gtest
)

target_include_directories(pylabhub_test_helpers
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/tests>
)

target_compile_features(pylabhub_test_helpers PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(pylabhub_test_helpers PRIVATE /Zc:preprocessor)
endif()
