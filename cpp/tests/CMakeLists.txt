# tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)
project(pylabhub-tests NONE)

# common include dir
set(PYLABHUB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# list of tests
set(TEST_SOURCES
    test_atomicguard.cpp
    test_logger.cpp
    test_filelock.cpp
    test_jsonconfig.cpp
)

foreach(src IN LISTS TEST_SOURCES)
    get_filename_component(tbl ${src} NAME_WE)
    add_executable(${tbl} ${src})
    target_include_directories(${tbl} PRIVATE ${PYLABHUB_INCLUDE_DIR})
    target_link_libraries(${tbl} PRIVATE pylabhub-util Threads::Threads)
    target_compile_features(${tbl} PRIVATE cxx_std_17)
    add_test(NAME ${tbl} COMMAND ${tbl})
endforeach()
