# tests/CMakeLists.txt - builds small unit test programs for AtomicGuard, FileLock, JsonConfig

cmake_minimum_required(VERSION 3.15)

# Create three small test executables that are independent and simple; no external test framework required.

# Helper: common include dir
set(PYLABHUB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# atomicguard test
add_executable(test_atomicguard test_atomicguard.cpp)
target_include_directories(test_atomicguard PRIVATE ${PYLABHUB_INCLUDE_DIR})
target_link_libraries(test_atomicguard PRIVATE pylabhub-util Threads::Threads)
target_compile_features(test_atomicguard PRIVATE cxx_std_17)
add_test(NAME atomicguard_test COMMAND test_atomicguard)

# filelock test
add_executable(test_filelock test_filelock.cpp)
target_include_directories(test_filelock PRIVATE ${PYLABHUB_INCLUDE_DIR})
target_link_libraries(test_filelock PRIVATE pylabhub-util Threads::Threads)
target_compile_features(test_filelock PRIVATE cxx_std_17)
add_test(NAME filelock_test COMMAND test_filelock)

# jsonconfig test
add_executable(test_jsonconfig test_jsonconfig.cpp)
target_include_directories(test_jsonconfig PRIVATE ${PYLABHUB_INCLUDE_DIR})
target_link_libraries(test_jsonconfig PRIVATE pylabhub-util Threads::Threads)
target_compile_features(test_jsonconfig PRIVATE cxx_std_17)
add_test(NAME jsonconfig_test COMMAND test_jsonconfig)
