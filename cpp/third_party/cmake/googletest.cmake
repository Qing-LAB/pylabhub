# third_party/cmake/googletest.cmake
#
# Wrapper for GoogleTest library.
#
# Exposes the following ALIAS targets for consumers:
#   - pylabhub::third_party::gtest      (gtest library)
#   - pylabhub::third_party::gtest_main (gtest_main library with main())
#   - pylabhub::third_party::gmock      (gmock library)
#   - pylabhub::third_party::gmock_main (gmock_main library with main())
#
# This wrapper ensures that GoogleTest is built as part of the project but not
# installed or staged, as it's a test-only dependency.

include(ThirdPartyPolicyAndHelper)

message(STATUS "[pylabhub-third-party] Configuring GoogleTest submodule...")

# Prevent gtest from being installed as part of our 'install' target.
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)

# Tell gtest to hide its symbols, which is good practice when embedding it
# and can prevent symbol clashes and build errors.
set(gtest_hide_internal_symbols ON CACHE BOOL "Hide gtest internal symbols" FORCE)

# Add the googletest subdirectory. This will define the `gtest` and `gtest_main` targets.
# The `EXCLUDE_FROM_ALL` argument ensures that gtest targets are only built if
# something (like our test executables) explicitly depends on them.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/googletest EXCLUDE_FROM_ALL)

# --- Expose GTest targets ---
add_library(pylabhub_gtest INTERFACE)
add_library(pylabhub::third_party::gtest ALIAS pylabhub_gtest)
target_link_libraries(pylabhub_gtest INTERFACE gtest)

add_library(pylabhub_gtest_main INTERFACE)
add_library(pylabhub::third_party::gtest_main ALIAS pylabhub_gtest_main)
target_link_libraries(pylabhub_gtest_main INTERFACE gtest_main)

# --- Expose GMock targets ---
add_library(pylabhub_gmock INTERFACE)
add_library(pylabhub::third_party::gmock ALIAS pylabhub_gmock)
target_link_libraries(pylabhub_gmock INTERFACE gmock)

add_library(pylabhub_gmock_main INTERFACE)
add_library(pylabhub::third_party::gmock_main ALIAS pylabhub_gmock_main)
target_link_libraries(pylabhub_gmock_main INTERFACE gmock_main)



message(STATUS "[pylabhub-third-party] GoogleTest configuration complete.")
