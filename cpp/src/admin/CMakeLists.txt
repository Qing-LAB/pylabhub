# /cpp/src/admin/CMakeLists.txt

cmake_minimum_required(VERSION 3.29)

add_executable(datablock-admin datablock_admin.cpp)

target_link_libraries(datablock-admin PRIVATE pylabhub-utils)

install(TARGETS datablock-admin DESTINATION ${CMAKE_INSTALL_BINDIR})
