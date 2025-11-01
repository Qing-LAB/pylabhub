# cpp/tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)

#
# tests CMake for pylabhub-shell
# - Links tests to pylabhub::util (alias to pylabhub-util)
# - Optionally links to libzmq if the top-level CMake set ZMQ_LIB_TARGET
#   (or if the common libzmq targets exist).
#

if(NOT TARGET pylabhub::util AND NOT TARGET pylabhub-util)
  message(FATAL_ERROR "pylabhub util library not found (expected pylabhub::util or pylabhub-util). Configure from parent CMakeLists.txt")
endif()

# Prefer the alias target if present
if(TARGET pylabhub::util)
  set(PYLABHUB_UTIL_TARGET pylabhub::util)
else()
  set(PYLABHUB_UTIL_TARGET pylabhub-util)
endif()

# Test source files
set(TEST_FILELOCK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test_filelock.cpp)
set(TEST_JSONCONFIG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test_jsonconfig.cpp)

# Create test executables
add_executable(test_filelock ${TEST_FILELOCK_SRC})
target_link_libraries(test_filelock PRIVATE ${PYLABHUB_UTIL_TARGET})

add_executable(test_jsonconfig ${TEST_JSONCONFIG_SRC})
target_link_libraries(test_jsonconfig PRIVATE ${PYLABHUB_UTIL_TARGET})

# -------------------------
# Optional libzmq linkage
# -------------------------
# The top-level CMake may set ZMQ_LIB_TARGET to a string name for the libzmq target
# (for example: "zmq::libzmq" or "libzmq"). If set and the target exists, link it.
# Otherwise, fall back to checking typical exported targets.
if(DEFINED ZMQ_LIB_TARGET AND ZMQ_LIB_TARGET)
  # ZMQ_LIB_TARGET was set by parent; link if that target exists
  if(TARGET ${ZMQ_LIB_TARGET})
    message(STATUS "tests: linking against provided ZMQ target: ${ZMQ_LIB_TARGET}")
    target_link_libraries(test_filelock PRIVATE ${ZMQ_LIB_TARGET})
    target_link_libraries(test_jsonconfig PRIVATE ${ZMQ_LIB_TARGET})
  else()
    message(WARNING "tests: ZMQ_LIB_TARGET='${ZMQ_LIB_TARGET}' set but no such target exists in this configure.")
  endif()
else()
  # Try common target names (safe-guard; won't error if none found)
  if(TARGET zmq::libzmq)
    message(STATUS "tests: linking against zmq::libzmq")
    target_link_libraries(test_filelock PRIVATE zmq::libzmq)
    target_link_libraries(test_jsonconfig PRIVATE zmq::libzmq)
  elseif(TARGET libzmq)
    message(STATUS "tests: linking against libzmq")
    target_link_libraries(test_filelock PRIVATE libzmq)
    target_link_libraries(test_jsonconfig PRIVATE libzmq)
  elseif(TARGET zmq)
    message(STATUS "tests: linking against zmq")
    target_link_libraries(test_filelock PRIVATE zmq)
    target_link_libraries(test_jsonconfig PRIVATE zmq)
  else()
    message(STATUS "tests: no libzmq target found — libzmq tests (if any) will be skipped or built without linking.")
  endif()
endif()

# -------------------------
# Windows: copy runtime DLLs if present in third_party/prebuilt
# -------------------------
if(WIN32)
  set(PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/prebuilt/windows")
  if(EXISTS "${PREBUILT_DIR}")
    file(GLOB_RECURSE PREBUILT_DLLS "${PREBUILT_DIR}/*/bin/*.dll")
    foreach(dll ${PREBUILT_DLLS})
      add_custom_command(TARGET test_filelock POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:test_filelock>"
        COMMENT "Copying ${dll} to test output dir"
      )
      add_custom_command(TARGET test_jsonconfig POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:test_jsonconfig>"
        COMMENT "Copying ${dll} to test output dir"
      )
    endforeach()
  endif()
endif()

# -------------------------
# UNIX: add RPATH entries if third_party libs were built to known locations
# (Optional and best-effort; adjust layout as needed)
# -------------------------
if(UNIX AND NOT APPLE)
  set(RPATH_LIST "")
  # Example: if libzmq was built in the top-level third_party build dir,
  # the parent CMake used: ${CMAKE_CURRENT_BINARY_DIR}/third_party/libzmq_build
  if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/third_party/libzmq_build")
    list(APPEND RPATH_LIST "${CMAKE_CURRENT_BINARY_DIR}/third_party/libzmq_build")
  endif()

  if(RPATH_LIST)
    string(REPLACE ";" ":" RPATH_JOINED "${RPATH_LIST}")
    set_target_properties(test_filelock PROPERTIES BUILD_RPATH "${RPATH_JOINED}")
    set_target_properties(test_jsonconfig PROPERTIES BUILD_RPATH "${RPATH_JOINED}")
    message(STATUS "tests: set BUILD_RPATH for tests to: ${RPATH_JOINED}")
  endif()
endif()

# -------------------------
# Register tests with CTest
# -------------------------
add_test(NAME filelock_test COMMAND test_filelock)
add_test(NAME jsonconfig_test COMMAND test_jsonconfig)

# Convenience target to run tests
add_custom_target(run_tests
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  DEPENDS test_filelock test_jsonconfig
)
