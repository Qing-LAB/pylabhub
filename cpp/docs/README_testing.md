# C++ Test Suite Architecture

This document outlines the architecture of the pyLabHub C++ test suite. Its goal is to ensure that tests are organized, scalable, and easy for developers to write and run.

## 1. High-Level Philosophy

Our test suite is built on three core principles:

1.  **Clarity**: Test code should be as readable and well-organized as the production code it validates.
2.  **Dependency Isolation**: Tests for a simple utility should not have dependencies on complex application logic. This keeps executables small and link times fast.
3.  **Speed**: A fast "inner loop" is critical for productivity. Developers must be able to run tests relevant to their changes quickly without waiting for a full suite build and run.

To achieve this, we use a **multiple-executable model** managed by CTest and GoogleTest.

## 2. Test Suite Structure

The test suite is not a single, monolithic application. It is composed of several distinct parts located in the `tests/` directory:

*   `pylabhub_test_helpers`: A static library (`tests/helpers/`) that contains shared code for all test executables. This includes the `main()` entry point, multi-process worker dispatch logic, and the implementations of all worker functions.
*   `utils_tests`: A test executable that contains all **single-process unit tests** for the `pylabhub::utils` library. These are typically fast-running tests that validate the logic of a single class in isolation.
*   `multiprocess_tests`: A test executable dedicated to tests that **must spawn child processes** to verify behavior. This is used for validating the cross-process safety of components like `FileLock`, `JsonConfig`, and the `Logger`.

This structure allows a developer working on the `utils` library to compile and run only the `utils_tests`, resulting in a much faster development cycle.

## 3. A Deep Dive: How Multi-Process Testing Works

The multi-process testing logic can seem complex at first, but it follows a consistent and powerful pattern. Understanding this flow is key to working with tests for components like `FileLock`.

The core idea is that a single test executable, like `multiprocess_tests`, has two personalities: it can act as a **"Parent"** (the test runner) or as a **"Worker"** (a child process performing a specific task).

### The Cast of Characters

*   **The Executable (`multiprocess_tests`):** The program CTest runs. It contains the code for both the parent test logic and all the worker functions.
*   **The Entry Point (`test_entrypoint.cpp`):** This file provides the `main()` function for the executable. It acts as a "router", deciding if the process should be a Parent or a Worker.
*   **The Parent Logic (`test_filelock.cpp`):** This is a standard `TEST_F()` block that defines the test steps, including when to spawn a child process.
*   **The Spawner (`test_process_utils.cpp`):** This contains the shared `spawn_worker_process()` helper function which handles the platform-specifics of creating a new process.
*   **The Worker Logic (`workers.cpp`):** This contains the actual code the child process will run, for example, `worker::filelock::nonblocking_acquire()`.

### Step-by-Step Execution Flow

This sequence diagram illustrates how the components interact when running a test like `TEST_F(FileLockTest, MultiProcessNonBlocking)`:

```mermaid
sequenceDiagram
    participant CTest
    participant Parent as multiprocess_tests (Parent)
    participant Child as multiprocess_tests (Child)

    Note over CTest, Parent: Step 1: CTest starts the test run.
    CTest->>Parent: Runs `multiprocess_tests`

    Note over Parent: Step 2: The Parent process starts.
    Parent->>Parent: main() in `test_entrypoint.cpp` is called.<br/>Sees NO worker arguments.<br/>Calls `RUN_ALL_TESTS()`.

    Note over Parent: Step 3: Google Test runs the specific test case.
    Parent->>Parent: Executes `TEST_F(FileLockTest, ...)`<br/>from `test_filelock.cpp`.

    Note over Parent, Child: Step 4: The Parent spawns a Child of itself.
    Parent->>Child: Calls `spawn_worker_process()` with args:<br/>- Path to `multiprocess_tests`<br/>- Mode: "filelock.nonblocking_acquire"

    Note over Child: Step 5: The Child process starts.
    Child->>Child: main() in `test_entrypoint.cpp` is called again.<br/>This time, it SEES the worker arguments.

    Note over Child: Step 6: The dispatcher routes to the correct worker.
    Child->>Child: The dispatcher parses "filelock.nonblocking_acquire"<br/>and calls the `worker::filelock::nonblocking_acquire()` function.

    Note over Child: Step 7: The Worker runs and exits.
    Child->>Child: The worker code runs its logic.<br/>The child process exits with a code (e.g., 0 for success).
    deactivate Child

    Note over Parent: Step 8: The Parent checks the result.
    Parent->>Parent: `waitpid()` inside the test completes.<br/>It checks the child's exit code.<br/>`ASSERT_EQ(exit_code, 0)` determines pass/fail.
```

## 4. How to Add a New Test

Follow the recipe that best matches your needs.

### Recipe 1: Add a Unit Test to an Existing Module

This is the most common scenario. You want to add a new test for a class that already has a test file.

1.  **Identify the Correct File**: Find the `test_*.cpp` file corresponding to your module (e.g., `tests/test_atomicguard.cpp` for `AtomicGuard`).
2.  **Add Your Test**: Add a new `TEST` or `TEST_F` block to the file, following the naming convention.

    ```cpp
    // In tests/test_atomicguard.cpp
    TEST(AtomicGuard, MyNewScenario_DoesTheRightThing)
    {
        // ... your test logic ...
        ASSERT_TRUE(someCondition);
    }
    ```
3.  **Done**: Because we explicitly list sources, re-run CMake to pick up the changes, then build and run the appropriate test executable (e.g., `utils_tests`).

### Recipe 2: Add Unit Tests for a New Module

You have created a new utility, `pylabhub::utils::MyNewUtil`, and need to add its first tests.

1.  **Create a New Test File**: Create `tests/test_mynewutil.cpp`.
2.  **Add Test Code**: Populate the file with a test fixture (if needed) and your `TEST` blocks.
3.  **Update CMake**: Add your new file to the appropriate test executable in `tests/CMakeLists.txt`. Since this is a single-process `utils` test, add it to `utils_tests`.

    ```cmake
    # In tests/CMakeLists.txt
    add_executable(utils_tests
      test_atomicguard.cpp
      test_logger.cpp
      test_recursionguard.cpp
      test_mynewutil.cpp  # <-- Add your new file here
    )
    ```

### Recipe 3: Add a New Multi-Process Test

This is the most involved scenario. Refer to the "Deep Dive" section above to understand the flow.

1.  **Define the Worker Logic** (`tests/helpers/workers.cpp`):
    Define a function for the child process inside the appropriate `worker::<module>` namespace.

    ```cpp
    // In tests/helpers/workers.cpp
    namespace worker::mymodule {
        int my_scenario(const std::string& arg1) {
            // ... logic for the child process ...
            return 0; // Success
        }
    }
    ```

2.  **Declare the Worker** (`tests/helpers/worker_mymodule.h`):
    Create a new header file for your module's workers and declare the function. Then, include this new header in the master `tests/helpers/workers.h`.

    ```cpp
    // In tests/helpers/worker_mymodule.h
    namespace worker::mymodule {
        int my_scenario(const std::string& arg1);
    }
    ```

3.  **Update the Dispatcher** (`tests/helpers/test_entrypoint.cpp`):
    Add an `else if` block to `main()` to recognize your new mode string (e.g., "mymodule.my_scenario") and call the worker function.

4.  **Write the Parent Test**:
    Create `tests/test_mymodule_multiprocess.cpp` and add it to the `multiprocess_tests` executable in `CMakeLists.txt`. In your `TEST` block, use the shared `spawn_worker_process()` helper to launch the child and check its exit code.

    ```cpp
    // In tests/test_mymodule_multiprocess.cpp
    #include "helpers/test_entrypoint.h" // For g_self_exe_path
    #include "helpers/test_process_utils.h"
    
    TEST(MyModule, MyMultiProcessScenario) {
        ProcessHandle child = spawn_worker_process(
            g_self_exe_path, 
            "mymodule.my_scenario", 
            { "argument1" }
        );
        // ... wait for child and assert exit code ...
    }
    ```

## 5. Naming and Style Conventions

*   **Test Cases**: Name test cases (`TEST`'s first argument) after the class or module being tested (e.g., `AtomicGuard`, `JsonConfig`).
*   **Test Names**: Name individual tests (`TEST`'s second argument) by describing the scenario and expected outcome, using `PascalCase` or `snake_case`.
*   **Fixtures**: For tests requiring common setup and teardown, use a test fixture (`class MyTest : public ::testing::Test`) and `TEST_F`.

## 6. How to Run Tests

All tests are managed by CTest. From your build directory:

*   **Run all tests**:
    ```bash
    ctest
    ```
*   **Run all tests in parallel**:
    ```bash
    ctest -j8
    ```
*   **Run only the `utils_tests` executable**:
    ```bash
    ctest -R utils_tests
    ```
*   **Run only the `AtomicGuard` tests within `utils_tests`**:
    ```bash
    ctest -R "utils_tests.AtomicGuard"
    ```
*   **Run a single, specific test**:
    ```bash
    ctest -R "utils_tests.AtomicGuard.MyNewScenario_DoesTheRightThing"
    ```

You can also run the executables directly with Google Test's `--gtest_filter` for more complex patterns:

```bash
# In ./build/bin/
./utils_tests --gtest_filter=AtomicGuard.*
```
