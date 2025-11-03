# third_party/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)

# Policy: do not install third_party libraries by default
option(THIRD_PARTY_INSTALL "Install third-party libraries" OFF)

# ---- fmt ----
# If fmt is present as a submodule, add it as a subdirectory so it defines targets
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fmt/CMakeLists.txt")
    # Preserve user choice about header-only usage
    if(USE_FMT_HEADER_ONLY)
        # when fmt header-only, define FMT_HEADER_ONLY for consumers; the fmt project
        # itself will honor FMT_HEADER_ONLY variable in some versions but we create an
        # interface target if needed.
        add_subdirectory(fmt EXCLUDE_FROM_ALL)
        # fmt provides targets fmt::fmt or fmt::fmt-header-only depending on version.
        # We will rely on fmt::fmt if available; otherwise fmt::fmt-header-only.
    else()
        add_subdirectory(fmt EXCLUDE_FROM_ALL)
    endif()
else()
    message(WARNING "fmt submodule missing: third_party/fmt not found")
endif()

# ---- libzmq (ZeroMQ) ----
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/CMakeLists.txt")
    # libzmq has CMake and will define targets, optionally build tests.
    add_subdirectory(libzmq EXCLUDE_FROM_ALL)
else()
    message(WARNING "libzmq submodule missing: third_party/libzmq not found")
endif()
