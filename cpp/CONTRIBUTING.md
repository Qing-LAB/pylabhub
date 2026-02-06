# Contributing to the pyLabHub C++ Project

First of all, thank you for considering contributing! This is a complex project with a specific architecture, and this guide is here to help you get started. Following these guidelines helps us maintain the quality and consistency of the codebase.

## Table of Contents

1.  [Code of Conduct](#code-of-conduct)
2.  [Getting Started](#getting-started)
    -   [Prerequisites](#prerequisites)
    -   [Building the Project](#building-the-project)
    -   [Running Tests](#running-tests)
3.  [Core Architectural Concepts](#core-architectural-concepts)
    -   [1. Dual Library Architecture](#1-dual-library-architecture)
    -   [2. Unified Staging Directory](#2-unified-staging-directory)
    -   [3. ABI Stability (The Pimpl Idiom)](#3-abi-stability-the-pimpl-idiom)
    -   [4. Application Lifecycle Management](#4-application-lifecycle-management)
    -   [5. CMake Alias Targets](#5-cmake-alias-targets)
4.  [Coding Style & Conventions](#coding-style--conventions)
    -   [C++ Standard](#c-standard)
    -   [Static Analysis with Clang-Tidy](#static-analysis-with-clang-tidy)
    -   [Formatting](#formatting)
5.  [How to Contribute](#how-to-contribute)
    -   [Adding a New Feature](#adding-a-new-feature)
    -   [Adding a New Test](#adding-a-new-test)
    -   [Git Workflow](#git-workflow)

---

## Code of Conduct

This project and everyone participating in it is governed by our Code of Conduct. By participating, you are expected to uphold this code. Please report unacceptable behavior.

*(Note: A formal Code of Conduct document should be added here.)*

---

## Getting Started

### Prerequisites

*   A C++20 compliant compiler (Clang, GCC, or MSVC).
*   CMake (version 3.29 or newer).
*   `clang-tidy` (optional, but highly recommended, as it's enabled by default).

### Building the Project

The project uses a "unified staging" build system. All artifacts are built and then copied to a local `build/stage` directory, creating a runnable package.

1.  **Configure CMake:**
    From the `cpp` directory, run:
    ```bash
    cmake -S . -B build
    ```

2.  **Build the code:**
    ```bash
    cmake --build build
    ```

3.  **Run the application:**
    Executables are placed in the staging directory.
    ```bash
    ./build/stage/bin/pylabhub-hubshell
    ```

### Running Tests

The test suite is managed by CTest. From your `build` directory:

*   **Run all tests:**
    ```bash
    ctest
    ```
*   **Run tests from a specific suite (e.g., all `FileLock` tests):**
    ```bash
    ctest -R "^FileLockTest"
    ```
*   **Run a single, specific test case:**
    ```bash
    ctest -R "^FileLockTest.MultiProcessExclusiveAccess$"
    ```

For more details, see the [Test Suite Architecture Guide](docs/README_testing.md).

---

## Core Architectural Concepts

This project has a few core design principles. Understanding them is key to contributing effectively.

### 1. Dual Library Architecture

The codebase is split into two primary libraries:

*   `pylabhub-basic` (`pylabhub::basic`): A **static library** containing foundational, low-level code (e.g., `RecursionGuard`, `platform` helpers). It has minimal dependencies and its code is compiled directly into the modules that use it.
*   `pylabhub-utils` (`pylabhub::utils`): A **shared library** for high-level, application-aware utilities (e.g., `Logger`, `FileLock`, `LifecycleManager`).

**Key Takeaway:** Code in `pylabhub-basic` cannot depend on `pylabhub-utils`.

### 2. Unified Staging Directory

Instead of a traditional `cmake --install` workflow for development, we use a **staging directory** inside the build folder (`build/stage`). All binaries, libraries, headers, and assets are copied here.

**Why?** This creates a self-contained, runnable version of the project that mirrors the final installation layout, making local development and testing simple and reliable.

### 3. ABI Stability (The Pimpl Idiom)

The `pylabhub-utils` library is a **shared library**, which means we must maintain a stable Application Binary Interface (ABI). A breaking ABI change would require all applications using the library to be recompiled.

To prevent this, we strictly adhere to the **Pointer to Implementation (Pimpl) idiom** for all public classes in the shared library.

*   **What it is:** The public class (e.g., `FileLock`) holds only a `std::unique_ptr` to a private, forward-declared `Impl` struct. All data members and private methods are placed in the `Impl` class, which is defined only in the `.cpp` file.
*   **Why?** This hides implementation details from the public header. You can add/remove private members and methods without changing the size or layout of the public class, thus preserving the ABI.

**Example: `file_lock.hpp`**
```cpp
// Public Header (file_lock.hpp)
class PYLABHUB_UTILS_EXPORT FileLock {
public:
    FileLock(const std::filesystem::path& path, ...);
    ~FileLock(); // Must be defined in the .cpp
    bool valid() const;
private:
    struct FileLockImpl; // Forward-declared, not defined
    std::unique_ptr<FileLockImpl, ...> pImpl; // The pointer to implementation
};
```

**Key Takeaway:** When modifying any class in `pylabhub-utils`, ensure that the public header remains ABI-stable. Add new private members to the `Impl` struct in the `.cpp` file, not to the public class declaration.

### 4. Application Lifecycle Management

High-level utilities in `pylabhub-utils` often require explicit startup and shutdown sequences (e.g., to start a logger thread or clean up lock files). The `pylabhub::utils::LifecycleManager` handles this.

*   **How it works:** Modules (`Logger`, `FileLock`) provide a `ModuleDef` that declares their dependencies and registers startup/shutdown callbacks.
*   **`LifecycleGuard`:** In `main()`, a single `LifecycleGuard` object is created. Its constructor initializes all modules in the correct dependency order, and its destructor shuts them all down gracefully.

**Key Takeaway:** If you create a new high-level utility that needs initialization, it should be integrated into the `LifecycleManager`. See `docs/README_utils.md` for a complete example.

### 5. CMake Alias Targets

Always link against the namespaced `ALIAS` targets, not the concrete target names.

*   **DO:** `target_link_libraries(my-target PRIVATE pylabhub::utils)`
*   **DON'T:** `target_link_libraries(my-target PRIVATE pylabhub-utils)`

**Why?** This provides an abstraction layer. The underlying target `pylabhub-utils` could be renamed or changed, but the `pylabhub::utils` alias will remain stable for all consumers.

---

## Coding Style & Conventions

### C++ Standard

The project uses **C++20**. All code should adhere to this standard.

### Static Analysis with Clang-Tidy

The project is configured to use `clang-tidy` for static analysis. This is enabled by default for all internal targets when using a Clang compiler. The configuration is in the root `.clang-tidy` file. Please ensure your contributions do not introduce new `clang-tidy` warnings.

### Formatting

The project uses `clang-format` to enforce a consistent code style. A `.clang-format` file is provided in the root directory. Please format your code before submitting a contribution.

*(Note: The `tools/format.sh` script can be used for this.)*

---

## How to Contribute

### Adding a New Feature

When adding a new feature, follow the "Developer's Cookbook" recipes in the [CMake Design Document](docs/README_CMake_Design.md). These provide step-by-step instructions for:

*   [Adding a New Executable](docs/README_CMake_Design.md#recipe-1-how-to-add-a-new-executable)
*   [Adding a New Shared Library](docs/README_CMake_Design.md#recipe-2-how-to-add-a-new-internal-shared-library)
*   [Adding a New Static Library](docs/README_CMake_Design.md#recipe-3-how-to-add-a-new-internal-static-library)

Remember to respect the distinction between the `pylabhub-basic` and `pylabhub-utils` libraries when deciding where to place your new code.

### Adding a New Test

New features should always be accompanied by tests.

1.  **Identify the right test executable:**
    *   For code in `pylabhub-basic`, add your test to `tests/test_pylabhub_corelib/`.
    *   For code in `pylabhub-utils`, add your test to `tests/test_pylabhub_utils/`.
2.  **Create your `test_my_feature.cpp` file.**
3.  **Add your file** to the `add_executable()` command in the appropriate `CMakeLists.txt`.
4.  **Write your test** using the GoogleTest framework.

### Git Workflow

1.  **Fork the repository.**
2.  **Create a new branch** for your feature or bugfix.
3.  **Commit your changes.** Write clear and descriptive commit messages.
4.  **Push your branch** to your fork.
5.  **Open a Pull Request** against the main repository.
6.  **Respond to feedback** from code review.

Thank you for contributing!
