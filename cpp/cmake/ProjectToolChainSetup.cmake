# ===========================================================================
# PHASE 1: Pre-Project Toolchain Configuration
#
# This phase runs BEFORE the `project()` command. Its sole purpose is to
# influence which compiler and toolchain CMake will select. Logic here cannot
# depend on compiler-specific information (like CMAKE_CXX_COMPILER_ID) because
# the compiler has not been tested yet. We must use general system variables
# like CMAKE_HOST_SYSTEM_NAME.
# ===========================================================================

# Include all user-facing options first, so they are available for the logic below.
include(ToplevelOptions)

# On macOS, ensure we use the Apple-provided toolchain.
if(FORCE_USE_CLANG_ON_APPLE AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  # For Makefile/Ninja generators, explicitly setting the compiler path
  # prevents CMake from finding conflicting versions (e.g., from Homebrew).
  if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    execute_process(COMMAND xcode-select -p OUTPUT_VARIABLE _xcode_dev_path RESULT_VARIABLE _xcode_select_res OUTPUT_STRIP_TRAILING_WHITESPACE)
    
    set(_clang_path_from_select "${_xcode_dev_path}/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang")
    set(_clangpp_path_from_select "${_xcode_dev_path}/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++")

    if(_xcode_select_res EQUAL 0 AND EXISTS "${_clang_path_from_select}" AND EXISTS "${_clangpp_path_from_select}")
      unset(ENV{CC})
      unset(ENV{CXX})
      set(CMAKE_C_COMPILER "${_clang_path_from_select}" CACHE FILEPATH "C compiler" FORCE)
      set(CMAKE_CXX_COMPILER "${_clangpp_path_from_select}" CACHE FILEPATH "C++ compiler" FORCE)
      message(STATUS "Top-level: forcing clang/clang++ via xcode-select on macOS: C=${_clang_path_from_select}, CXX=${_clangpp_path_from_select}")
    else()
      message(WARNING "Top-level: could not find clang/clang++ via xcode-select. Falling back to xcrun.")
      execute_process(COMMAND xcrun --find clang OUTPUT_VARIABLE _clang_path RESULT_VARIABLE _xcrun_res_c OUTPUT_STRIP_TRAILING_WHITESPACE)
      execute_process(COMMAND xcrun --find clang++ OUTPUT_VARIABLE _clangpp_path RESULT_VARIABLE _xcrun_res_cxx OUTPUT_STRIP_TRAILING_WHITESPACE)
    
      if(_xcrun_res_c EQUAL 0 AND _xcrun_res_cxx EQUAL 0 AND EXISTS "${_clang_path}" AND EXISTS "${_clangpp_path}")
        set(CMAKE_C_COMPILER "${_clang_path}" CACHE FILEPATH "C compiler" FORCE)
        set(CMAKE_CXX_COMPILER "${_clangpp_path}" CACHE FILEPATH "C++ compiler" FORCE)
        message(STATUS "Top-level: forcing clang/clang++ via xcrun on macOS: ${_clang_path}")
      else()
        message(WARNING "Top-level: could not find clang/clang++ via xcrun. Falling back to find_program.")
        find_program(_clang_path clang HINTS /usr/bin)
        find_program(_clangpp_path clang++ HINTS /usr/bin)
        if(_clang_path AND _clangpp_path)
          set(CMAKE_C_COMPILER "${_clang_path}" CACHE FILEPATH "C compiler" FORCE)
          set(CMAKE_CXX_COMPILER "${_clangpp_path}" CACHE FILEPATH "C++ compiler" FORCE)
          message(STATUS "Top-level: forcing clang/clang++ on macOS: ${_clang_path}")
        else()
          message(WARNING "Top-level: clang not found on PATH. Consider installing Xcode or CommandLineTools.")
        endif()
      endif()
    endif()
  # For the Xcode generator, we must NOT set CMAKE_C_COMPILER directly. Instead, we
  # unset environment variables that might confuse `xcodebuild` at build time.
  else()
    unset(ENV{CC})
    unset(ENV{CXX})
    message(STATUS "Xcode generator: Unsetting CC and CXX environment variables to allow default toolchain selection.")
  endif()
endif()