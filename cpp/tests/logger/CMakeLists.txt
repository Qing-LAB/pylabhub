# tests/logger/CMakeLists.txt

# This file is part of the test suite refactoring.
# It collects all the individual test source files for the logger test
# and adds them to the parent's test executable target.

# Find all the source files in this directory.
file(GLOB logger_test_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# The "test_logger" executable target is defined in the parent CMakeLists.txt.
# We add our collected source files to it.
target_sources(test_logger
  PRIVATE
    ${logger_test_sources}
)
