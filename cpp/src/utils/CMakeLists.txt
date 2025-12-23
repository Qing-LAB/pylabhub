# /cpp/src/utils/CMakeLists.txt
#
# Configures the pylabhub-utils shared library.
# This library contains all common utilities for the project.
#
cmake_minimum_required(VERSION 3.18)

# Define the source files for the utility library explicitly.
# This is more robust than using file(GLOB), as it ensures that adding or
# removing source files requires a direct, version-controlled change to the
# build script. This prevents "silent" build failures where new files are not
# automatically compiled.
set(UTILS_SOURCES
  Lifecycle.cpp
  Logger.cpp
  JsonConfig.cpp
  FileLock.cpp
  AtomicGuard.cpp
  # RecursionGuard.cpp is now a header-only utility.
  # Add any new .cpp files for this library to the list above.
)

# Define the shared library target.
add_library(pylabhub-utils SHARED ${UTILS_SOURCES})
add_library(pylabhub::utils ALIAS pylabhub-utils)

# Set modern C++ standard.
target_compile_features(pylabhub-utils PUBLIC cxx_std_20)

# ==============================================================================
# Symbol Export and Visibility Control
#
# Use GenerateExportHeader to create export macros for this library.
# This ensures that symbols are correctly exported from the DLL on Windows and
# that visibility is managed on other platforms.
include(GenerateExportHeader)
generate_export_header(pylabhub-utils
  BASE_NAME "pylabhub_utils"
  EXPORT_MACRO_NAME "PYLABHUB_UTILS_EXPORT"
)

# By default, we want all symbols to be hidden unless explicitly exported.
# On Windows, this is the default for DLLs.
# On GCC/Clang, we must explicitly set the compiler flag.
if(NOT MSVC)
  target_compile_options(pylabhub-utils PRIVATE -fvisibility=hidden)
endif()

# The generated header needs to be on the include path. Since this is a public
# dependency for consumers of the library, we make it a PUBLIC include.
# The generated header is placed in the build directory for the current source
# directory.
target_include_directories(pylabhub-utils
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

# Ensure the generated header is available to consumers
target_compile_definitions(pylabhub-utils PRIVATE PYLABHUB_UTILS_EXPORT_DLL=1)


# ==============================================================================

# Check if we are using MSVC
if(MSVC)
    # Enable synchronous exception handling and disable warning C5105.
    target_compile_options(pylabhub-utils PRIVATE /EHsc /wd5105)

    # Enable the standards-conforming preprocessor
    target_compile_options(pylabhub-utils PRIVATE /Zc:preprocessor)
endif()

if (PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(pylabhub-utils PRIVATE _LOGGER_DEBUG_ENABLED=1)
    message(STATUS "Building pylabhub-utils with logger debug enabled.")
endif()

# Define the public include directory for consumers of this library.
# This makes `include/` available via `target_include_directories`.
target_include_directories(pylabhub-utils
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Link against necessary third-party libraries and system libraries.
target_link_libraries(pylabhub-utils
  PUBLIC
    pylabhub::third_party::fmt
    pylabhub::third_party::nlohmann_json
    Threads::Threads
)

# If tests are being built, we need to compile the test-only reset logic
# into this library so it can be exported and used by the test executables.
if(BUILD_TESTS)
  target_compile_definitions(pylabhub-utils PRIVATE PYLABHUB_TESTING)
  message(STATUS "pylabhub-utils: Compiling with test-only reset logic enabled.")
endif()

# ---------- Probe: Detect fmt::chrono subsecond formatting style ----------
if (TARGET pylabhub-utils)
  message(STATUS "Performing test: probing fmt chrono subseconds support...")
  if (CMAKE_CROSSCOMPILING)
    message(STATUS "Cross-compiling: skipping fmt chrono subseconds runtime probe; defaulting to fallback (no subseconds).")
    set(HAVE_FMT_CHRONO_SUBSECONDS 0 CACHE INTERNAL "fmt chrono subseconds support")
    set(FMT_CHRONO_FMT_STYLE 0 CACHE INTERNAL "fmt chrono format style")
  else()
    # Get fmt include directories from the fmt::fmt target
    if (TARGET fmt::fmt)
      get_target_property(_fmt_includes fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)
    else()
      set(_fmt_includes "${CMAKE_SOURCE_DIR}/third_party/fmt/include")
    endif()

    # Compose the C++ source for the probe.
    set(_probe_src_content [==[
#include <chrono>
#include <iostream>
#include <regex>
#include <string>
#include <fmt/format.h>
#include <fmt/chrono.h>
int main() {
    using namespace std::chrono;
    try {
        auto now = system_clock::now();
        auto tp_us = time_point_cast<microseconds>(now);

        bool ok_with_f = false;
        try {
            std::string s = fmt::format("{:%Y-%m-%d %H:%M:%S.%f}", tp_us);
            std::regex frac_re("\\.[0-9]{1,9}");
            if (std::regex_search(s, frac_re)) {
                std::cout << "WITH_F:OK\n" << s << std::endl;
                ok_with_f = true;
            } else {
                std::cerr << "WITH_F:NO_FRAC -- got: " << s << std::endl;
            }
        } catch (const std::exception &e) { std::cerr << "WITH_F:EXCEPTION: " << e.what() << std::endl; }
        catch (...) { std::cerr << "WITH_F:UNKNOWN_EXCEPTION\n"; }

        bool ok_without_f = false;
        try {
            std::string s2 = fmt::format("{:%Y-%m-%d %H:%M:%S}", tp_us);
            std::regex frac_re("\\.[0-9]{1,9}");
            if (std::regex_search(s2, frac_re)) {
                std::cout << "WITHOUT_F:OK\n" << s2 << std::endl;
                ok_without_f = true;
            } else {
                std::cerr << "WITHOUT_F:NO_FRAC -- got: " << s2 << std::endl;
            }
        } catch (const std::exception &e) { std::cerr << "WITHOUT_F:EXCEPTION: " << e.what() << std::endl; }
        catch (...) { std::cerr << "WITHOUT_F:UNKNOWN_EXCEPTION\n"; }

        if (ok_with_f) { std::cout << "DECISION: STYLE=WITH_F\n"; return 0; }
        if (ok_without_f) { std::cout << "DECISION: STYLE=WITHOUT_F\n"; return 2; }
        std::cerr << "DECISION: STYLE=NONE\n"; return 1;
    } catch (const std::exception &e) {
        std::cerr << "PROBE_TOP_EXCEPTION: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "PROBE_TOP_UNKNOWN_EXCEPTION\n";
        return 1;
    }
}
]==])

    # try_run is the standard CMake way to compile and run a test executable at configure time.
    # We pass the include directories for fmt via CMAKE_FLAGS, which the inner CMake run will use.
    set(_probe_file "${CMAKE_CURRENT_BINARY_DIR}/fmt_chrono_subsec_probe.cpp")
    file(WRITE "${_probe_file}" "${_probe_src_content}")
    try_run(
      FMT_PROBE_RUN_RESULT
      FMT_PROBE_COMPILE_RESULT
      SOURCES "${_probe_file}"
      CMAKE_FLAGS "-DINCLUDE_DIRECTORIES:STRING=${_fmt_includes}"
      COMPILE_DEFINITIONS -DFMT_HEADER_ONLY
      COMPILE_OUTPUT_VARIABLE FMT_PROBE_COMPILE_OUTPUT
      RUN_OUTPUT_VARIABLE FMT_PROBE_RUN_OUTPUT)

    if(FMT_PROBE_COMPILE_RESULT)
      if(FMT_PROBE_RUN_RESULT EQUAL 0)
        message(STATUS "    ** fmt chrono: sub-second support detected, format microsecond with '%f'.")
        message(VERBOSE "        Probe run output:\n\n${FMT_PROBE_RUN_OUTPUT}\n")
        set(HAVE_FMT_CHRONO_SUBSECONDS 1 CACHE INTERNAL "fmt chrono subseconds support")
        set(FMT_CHRONO_FMT_STYLE 1 CACHE INTERNAL "fmt chrono format style is %f")
      elseif(FMT_PROBE_RUN_RESULT EQUAL 2)
        message(STATUS "    ** fmt chrono: sub-second support detected, format microsecond without '%f'.")
        message(VERBOSE "        Probe run output:\n\n${FMT_PROBE_RUN_OUTPUT}\n")
        set(HAVE_FMT_CHRONO_SUBSECONDS 1 CACHE INTERNAL "fmt chrono subseconds support")
        set(FMT_CHRONO_FMT_STYLE 2 CACHE INTERNAL "fmt chrono format style is implicit")
      else()
        message(STATUS "    ** fmt chrono: sub-second support not available.")
        message(VERBOSE "        Probe returned: ${FMT_PROBE_RUN_RESULT}")
        message(VERBOSE "        Probe run output:\n\n${FMT_PROBE_RUN_OUTPUT}\n")
        set(HAVE_FMT_CHRONO_SUBSECONDS 0 CACHE INTERNAL "fmt chrono subseconds support")
        set(FMT_CHRONO_FMT_STYLE 0 CACHE INTERNAL "fmt chrono format style")
      endif()
    else()
      message(WARNING "     ** fmt chrono: probe failed to compile. Assuming no sub-second support.")
      message(VERBOSE "        Compiler output:\n\n${FMT_PROBE_COMPILE_OUTPUT}")
      set(HAVE_FMT_CHRONO_SUBSECONDS 0 CACHE INTERNAL "fmt chrono subseconds support")
      set(FMT_CHRONO_FMT_STYLE 0 CACHE INTERNAL "fmt chrono format style")
    endif()
  endif()

  target_compile_definitions(pylabhub-utils
    PRIVATE
    HAVE_FMT_CHRONO_SUBSECONDS=${HAVE_FMT_CHRONO_SUBSECONDS}
    FMT_CHRONO_FMT_STYLE=${FMT_CHRONO_FMT_STYLE})
endif()
# ---------- End probe ----------



# ==============================================================================
# Installation
#
# Defines the rules for installing this library as part of a package that can
# be used by other CMake projects via `find_package`.
# ==============================================================================
install(TARGETS pylabhub-utils
  EXPORT pylabhubTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Install the public headers for this library.
# The `pylabhub_utils_export.h` header is essential for consumers.
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/pylabhub_utils_export.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/utils/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/utils
)

# --- Staging ---
# Define a local custom target to handle staging for this component.
message(STATUS "Configuring staging for pylabhub-utils library.")
pylabhub_get_library_staging_commands(
  TARGET pylabhub-utils
  DESTINATION bin # Shared libraries go to bin/
  OUT_COMMANDS stage_commands
)
# # Define another staging command for tests that may need this library.
# # The tests are placed in a configuration-specific subdirectory.
# message(STATUS "Configuring staging for pylabhub-utils library for tests.")
# pylabhub_get_library_staging_commands(
#   TARGET pylabhub-utils
#   DESTINATION "${CMAKE_BINARY_DIR}/bin/${CMAKE_CONFIGURATION_TYPES}" # Shared libraries go to where the tests are first placed during build
#   OUT_COMMANDS stage_commands_for_tests
#   ABSOLUTE_DIR ON
# )

add_custom_target(stage_pylabhub_utils ${stage_commands} COMMENT "Staging pylabhub-utils library")
# add_custom_target(stage_pylabhub_utils_for_tests ${stage_commands_for_tests} COMMENT "Staging pylabhub-utils library for tests")
add_dependencies(stage_pylabhub_utils pylabhub-utils create_staging_dirs)
# add_dependencies(stage_pylabhub_utils_for_tests pylabhub-utils)

# Also stage the generated export header alongside the library.
add_custom_command(TARGET stage_pylabhub_utils POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_BINARY_DIR}/pylabhub_utils_export.h"
          "${PYLABHUB_STAGING_DIR}/include/"
  COMMENT "Staging generated export header for pylabhub-utils"
)

# Stage the public headers for this library to match the installation.
add_custom_command(TARGET stage_pylabhub_utils POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/include/utils"
          "${PYLABHUB_STAGING_DIR}/include/utils"
  COMMENT "Staging public headers for pylabhub-utils"
)

# Register this local staging target with the global build system.
set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_pylabhub_utils)

message(STATUS "Configured shared library 'pylabhub::utils'.")