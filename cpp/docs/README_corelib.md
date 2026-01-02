# C++ Core Library (`pylabhub-basic`) Documentation

This document provides design and usage notes for the foundational C++ components found in the `pylabhub-basic` static library. The components are primarily located within the `pylabhub::basics`, `pylabhub::platform`, and `pylabhub::format_tools` C++ namespaces.

---

## 1. Core Library Design

`pylabhub-basic` is the foundational, **static library** of the pyLabHub C++ application. It contains essential, low-level utilities and interfaces that are broadly used across the entire codebase. In the build system, it is exposed via the `pylabhub::basic` CMake alias target.

### Core Design Principles

*   **Static Linking**: As a static library, its code is compiled directly into the modules that depend on it (like the `pylabhub-hubshell` executable and the `pylabhub-utils` shared library). This ensures these fundamental tools are always available without requiring a separate shared library to be distributed.
*   **Minimal Dependencies**: This library is designed to have minimal external dependencies. It does **not** depend on the `pylabhub-utils` shared library.
*   **Header-Focused**: The library provides key functionality through its public headers located in the top-level `include/` directory.

### Key C++ Components by Namespace

The `pylabhub-basic` library provides the following key components, organized by their C++ namespace:

*   **`pylabhub::basics` namespace**:
    *   `AtomicGuard`: A high-performance, token-based guard for managing exclusive resource access.
    *   `RecursionGuard`: A utility to prevent re-entrant function calls within the same thread.
    *   `ScopeGuard`: A simple, general-purpose RAII guard that executes a function upon scope exit.
*   **`pylabhub::format_tools` namespace**:
    *   A collection of utilities and template specializations that integrate with the `fmt` library to provide custom formatting for project-specific types.
*   **`pylabhub::platform` namespace**:
    *   Contains platform-specific macros, type definitions, and functions to abstract away differences between operating systems.

---

## 2. Developer's Guide

### When to Add Code to `pylabub::basic`

A piece of code is a candidate for the core library if it meets these criteria:

1.  **Is it foundational?** Is it a low-level utility or data structure that could be needed by *any* other part of the application, including the `pylabub::utils` library?
2.  **Does it have minimal dependencies?** It should not depend on any component from `pylabub::utils`.
3.  **Is it suitable for static linking?** It doesn't manage a global state that would be problematic if duplicated across different shared libraries (though this is less of an issue in our current architecture).

**Good Candidates**:
*   A custom string manipulation function.
*   A cross-platform file system helper.
*   A new, general-purpose concurrency primitive.

**Bad Candidates**:
*   A class that requires application lifecycle management (depends on `utils::Lifecycle`).
*   A utility that logs messages using `utils::Logger`.
*   Anything that requires a concrete implementation from the `pylabub::utils` shared library.

### How to Add a New Component

1.  **Place Headers**: Add your new header file (e.g., `my_utility.hpp`) to the `include/` directory.
2.  **Place Source**: Add the corresponding source file (e.g., `my_utility.cpp`) to the `src/lib/` directory.
3.  **No CMake Changes Needed**: The `src/CMakeLists.txt` file is configured to automatically discover and compile all `.cpp` files within the `src/` directory (excluding specific sub-project directories). Your new component will be automatically included in the `pylabub::basic` static library during the next build.
