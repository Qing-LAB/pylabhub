# cmake/FmtChronoProbe.cmake
#
# Probes the fmt library to detect how it handles subsecond formatting
# in chrono time points.

message(STATUS "[pylabhub-fmt-probe] Performing test: probing fmt chrono subseconds support...")
if (CMAKE_CROSSCOMPILING)
  message(STATUS "[pylabhub-fmt-probe] Cross-compiling: skipping fmt chrono subseconds runtime probe; defaulting to fallback (no subseconds).")
  set(HAVE_FMT_CHRONO_SUBSECONDS 0 CACHE INTERNAL "fmt chrono subseconds support")
else()
  # Check if the probe has already been run and its results cached.
  if (DEFINED HAVE_FMT_CHRONO_SUBSECONDS AND DEFINED FMT_CHRONO_FMT_STYLE)
    message(STATUS "[pylabhub-fmt-probe] Probe results found in cache. Skipping re-run.")
    message(STATUS "[pylabhub-fmt-probe] HAVE_FMT_CHRONO_SUBSECONDS = ${HAVE_FMT_CHRONO_SUBSECONDS}")
    message(STATUS "[pylabhub-fmt-probe] FMT_CHRONO_FMT_STYLE = ${FMT_CHRONO_FMT_STYLE}")
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
      message(STATUS "[pylabhub-fmt-probe] Sub-second support detected, format microsecond with '%f'.")
      message(VERBOSE "        Probe run output:\n\n${FMT_PROBE_RUN_OUTPUT}\n")
      set(HAVE_FMT_CHRONO_SUBSECONDS 1 CACHE INTERNAL "fmt chrono subseconds support")
      set(FMT_CHRONO_FMT_STYLE 1 CACHE INTERNAL "fmt chrono format style is %f")
    elseif(FMT_PROBE_RUN_RESULT EQUAL 2)
      message(STATUS "[pylabhub-fmt-probe] Sub-second support detected, format microsecond without '%f'.")
      message(VERBOSE "        Probe run output:\n\n${FMT_PROBE_RUN_OUTPUT}\n")
      set(HAVE_FMT_CHRONO_SUBSECONDS 1 CACHE INTERNAL "fmt chrono subseconds support")
      set(FMT_CHRONO_FMT_STYLE 2 CACHE INTERNAL "fmt chrono format style is implicit")
    else()
      message(STATUS "[pylabhub-fmt-probe] Sub-second support not available.")
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
  endif() # Closes the `if(FMT_PROBE_COMPILE_RESULT)` block
  endif() # Closes the `else()` block for caching
endif() # Closes the `else()` block for `CMAKE_CROSSCOMPILING`
