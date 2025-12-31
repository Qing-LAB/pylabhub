# tests/logger/CMakeLists.txt

# This file is part of the test suite refactoring.
# It collects all the individual test source files for the logger test
# and adds them to the parent's test executable target.

# Find all the source files in this directory.
file(GLOB test_corelib_srcs
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# The "test_logger" executable target is defined in the parent CMakeLists.txt.
# We add our collected source files to it.
target_sources(test_corelib
  PRIVATE
    ${test_corelib_srcs}
)
target_compile_features(test_corelib PRIVATE cxx_std_20)
target_link_libraries(test_corelib
  PRIVATE
    pylabhub_test_helpers
)



