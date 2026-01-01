// tests/test_harness/shared_test_helpers.h
#pragma once

#include <gtest/gtest.h>
#include <fmt/core.h>
#include <chrono>
#include <filesystem>
#include <string>

#include "utils/Lifecycle.hpp"
#include "scope_guard.hpp"

namespace fs = std::filesystem;

/**
 * @file shared_test_helpers.h
 * @brief Provides common helper functions and utilities for test cases.
 *
 * This includes file I/O helpers, test scaling utilities, and a generic
 * wrapper for running test logic within a worker process, ensuring proper

 * lifecycle management and exception handling.
 */
namespace pylabhub::tests::helper
{

    /**
     * @brief Reads the entire contents of a file into a string.
     * @param path The path to the file.
     * @param out A reference to a string where the contents will be stored.
     * @return True if the file was read successfully, false otherwise.
     */
    bool read_file_contents(const std::string &path, std::string &out);

    /**
     * @brief Counts the number of newline characters in a string.
     * @param s The string to process.
     * @return The number of lines.
     */
    size_t count_lines(const std::string &s);

    /**
     * @brief Waits for a specific string to appear in a file.
     *
     * Polls a file until the expected string is found or a timeout is reached.
     *
     * @param path The path to the file to monitor.
     * @param expected The string to search for.
     * @param timeout The maximum time to wait.
     * @return True if the string was found, false if the timeout was reached.
     */
    bool wait_for_string_in_file(const fs::path &path, const std::string &expected,
                                        std::chrono::milliseconds timeout = std::chrono::seconds(15));

    /**
     * @brief Retrieves the test scale factor from the environment.
     *
     * Used to run shorter/lighter tests in certain environments (e.g., CI).
     * Set the `PYLAB_TEST_SCALE` environment variable to "small".
     *
     * @return The value of the `PYLAB_TEST_SCALE` environment variable, or an empty string.
     */
    std::string test_scale();

    /**
     * @brief Returns a value based on the current test scale.
     * @param original The value to use for a full-scale test.
     * @param small_value The value to use for a "small" scale test.
     * @return `small_value` if `test_scale()` returns "small", otherwise `original`.
     */
    int scaled_value(int original, int small_value);

    /**
     * @brief A template function to wrap test logic for execution in a worker process.
     *
     * This function handles the initialization and finalization of lifecycle-managed
     * modules and catches GTest assertions and standard exceptions, printing
     * informative error messages if the test logic fails.
     *
     * @tparam Fn The type of the test logic callable.
     * @tparam Mods The types of the lifecycle modules to manage.
     * @param test_logic The lambda or function containing the test assertions.
     * @param test_name A descriptive name for the test, used in error messages.
     * @param mods The lifecycle modules (e.g., `Logger::GetLifecycleModule()`) required by the test.
     * @return 0 on success, 1 on GTest assertion failure, 2 on standard exception, 3 on unknown exception.
     */
    template <typename Fn, typename... Mods>
    int run_gtest_worker(Fn test_logic, const char *test_name, Mods&&... mods)
    {
        pylabhub::lifecycle::LifecycleGuard guard(std::forward<Mods>(mods)...);

        try
        {
            test_logic();
        }
        catch (const ::testing::AssertionException &e)
        {
            fmt::print(stderr, "[WORKER FAILURE] GTest assertion failed in {}: \n{}\n", test_name,
                       e.what());
            return 1;
        }
        catch (const std::exception &e)
        {
            fmt::print(stderr, "[WORKER FAILURE] {} threw an exception: {}\n", test_name, e.what());
            return 2;
        }
        catch (...)
        {
            fmt::print(stderr, "[WORKER FAILURE] {} threw an unknown exception.\n", test_name);
            return 3;
        }
        return 0; // Success
    }

} // namespace pylabhub::tests::helper