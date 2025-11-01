# cpp/tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)

if(NOT TARGET pylabhub-util)
  message(FATAL_ERROR "pylabhub-util target not found. Please configure from the parent CMakeLists.txt.")
endif()

# Test sources
set(TEST_FILELOCK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test_filelock.cpp)
set(TEST_JSONCONFIG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test_jsonconfig.cpp)

add_executable(test_filelock ${TEST_FILELOCK_SRC})
target_link_libraries(test_filelock PRIVATE pylabhub-util)

add_executable(test_jsonconfig ${TEST_JSONCONFIG_SRC})
target_link_libraries(test_jsonconfig PRIVATE pylabhub-util)

# If libzmq was added as a submodule and produced a target, link to it for tests that use it.
# Common exported names include: zmq::libzmq, libzmq, zmq
if(TARGET zmq::libzmq)
  target_link_libraries(test_filelock PRIVATE zmq::libzmq)
  target_link_libraries(test_jsonconfig PRIVATE zmq::libzmq)
elseif(TARGET libzmq)
  target_link_libraries(test_filelock PRIVATE libzmq)
  target_link_libraries(test_jsonconfig PRIVATE libzmq)
elseif(TARGET zmq)
  target_link_libraries(test_filelock PRIVATE zmq)
  target_link_libraries(test_jsonconfig PRIVATE zmq)
endif()

# Windows: copy any runtime DLLs from third_party/prebuilt into the test exe dir
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

# On Linux/macOS, set RUNPATH so tests can find libs in third_party build dir (optional)
if(UNIX AND NOT APPLE)
  # If third_party libs are built into ${CMAKE_BINARY_DIR}/third_party/<name>/lib, add it
  set(THIRD_PARTY_LIB_DIRS "")
  if(EXISTS "${CMAKE_BINARY_DIR}/third_party/libzmq")
    list(APPEND THIRD_PARTY_LIB_DIRS "${CMAKE_BINARY_DIR}/third_party/libzmq")
  endif()

  if(THIRD_PARTY_LIB_DIRS)
    foreach(d ${THIRD_PARTY_LIB_DIRS})
      get_filename_component(d ${d} ABSOLUTE)
      # add rpath so test executables find shared libs at runtime
      set_target_properties(test_filelock PROPERTIES BUILD_RPATH "${d}")
      set_target_properties(test_jsonconfig PROPERTIES BUILD_RPATH "${d}")
    endforeach()
  endif()
endif()

# Register tests with CTest
add_test(NAME filelock_test COMMAND test_filelock)
add_test(NAME jsonconfig_test COMMAND test_jsonconfig)

# Convenience target
add_custom_target(run_tests
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  DEPENDS test_filelock test_jsonconfig
)

