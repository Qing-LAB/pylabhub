# luajit.cmake
include(ThirdPartyPolicyAndHelper)

if(NOT PREREQ_INSTALL_DIR)
  set(PREREQ_INSTALL_DIR "${CMAKE_BINARY_DIR}/prereqs")
endif()

set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/luajit")
set(_build_dir "${CMAKE_BINARY_DIR}/third_party/luajit-build")
set(_install_dir "${PREREQ_INSTALL_DIR}")

# For Makefile-based projects (LuaJIT), we will call configure/make/install directly.
# The pylabhub_add_external_prerequisite macro uses CMake-based CONFIGURE by default.
# Instead, add an ExternalProject manually using the same sanitization function.

pylabhub_sanitize_compiler_flags("CMAKE_C_FLAGS" _clean_c_flags)
pylabhub_sanitize_compiler_flags("CMAKE_CXX_FLAGS" _clean_cxx_flags)

ExternalProject_Add(luajit_external
  SOURCE_DIR "${_source_dir}"
  BINARY_DIR "${_build_dir}"
  DOWNLOAD_COMMAND ""
  # Copy source to binary dir to keep out-of-source semantics for in-place builds
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E copy_directory "${_source_dir}" "${_build_dir}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${_build_dir}" sh ./configure PREFIX=${_install_dir} CC="${CMAKE_C_COMPILER}" CFLAGS="${_clean_c_flags}"
  BUILD_COMMAND ${CMAKE_COMMAND} -E chdir "${_build_dir}" ${CMAKE_MAKE_PROGRAM}
  INSTALL_COMMAND ${CMAKE_COMMAND} -E chdir "${_build_dir}" ${CMAKE_MAKE_PROGRAM} install
  COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake"
  BUILD_BYPRODUCTS "${_install_dir}/luajit-stamp.txt"
)

# write a detect script for luajit similar to others (or rely on generic detect template)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "set(PACKAGE_NAME \"luajit\")\n")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "set(PREREQ_INSTALL_DIR \"${_install_dir}\")\n")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "set(PACKAGE_BINARY_DIR \"${_build_dir}\")\n")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "set(STABLE_BASENAME \"luajit-stable\")\n")
file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/detect_luajit.cmake" "include(\"${CMAKE_CURRENT_LIST_DIR}/detect_external_project.cmake.in\")\n")

add_library(pylabhub::third_party::luajit UNKNOWN IMPORTED GLOBAL)
set_target_properties(pylabhub::third_party::luajit PROPERTIES
  IMPORTED_LOCATION "${_install_dir}/lib/luajit-stable" # detect script will add ext
  INTERFACE_INCLUDE_DIRECTORIES "${_install_dir}/include"
)
add_dependencies(pylabhub::third_party::luajit luajit_external)
add_library(luajit::pylabhub ALIAS pylabhub::third_party::luajit)
