// tests/test_harness/shared_test_helpers.h
#pragma once

#include <chrono>
#include <filesystem>
#include <fmt/core.h>
#include <gtest/gtest.h>
#include <string>

#include "debug_info.hpp"
#include "platform.hpp"
#include "scope_guard.hpp"
#include "utils/Lifecycle.hpp"

namespace fs = std::filesystem;

/**
 * @file shared_test_helpers.h
 * @brief Provides common helper functions and utilities for test cases.
 *
 * This includes file I/O helpers, test scaling utilities, and a generic
 * wrapper for running test logic within a worker process, ensuring proper

 * lifecycle management and exception handling.
 */

#if PYLABHUB_IS_POSIX
#include <fcntl.h>
#include <unistd.h>
#else              // Windows
#include <cstdio>  // for _fileno, stderr
#include <fcntl.h> // For _O_BINARY
#include <io.h>
#define STDERR_FILENO _fileno(stderr)
typedef int ssize_t;
#endif

namespace pylabhub::tests::helper
{

class StringCapture
{
  public:
    explicit StringCapture(int fd_to_capture) : fd_to_capture_(fd_to_capture), original_fd_(-1)
    {
#if PYLABHUB_IS_POSIX
        if (pipe(pipe_fds_) != 0)
            return;
        original_fd_ = dup(fd_to_capture_);
        dup2(pipe_fds_[1], fd_to_capture_);
        close(pipe_fds_[1]);
#else // Windows
        if (_pipe(pipe_fds_, 1024, _O_BINARY) != 0)
            return;
        original_fd_ = _dup(fd_to_capture_);
        _dup2(pipe_fds_[1], fd_to_capture_);
        _close(pipe_fds_[1]);
#endif
    }

    ~StringCapture()
    {
        if (original_fd_ != -1)
        {
#if PYLABHUB_IS_POSIX
            dup2(original_fd_, fd_to_capture_);
            close(original_fd_);
#else // Windows
            _dup2(original_fd_, fd_to_capture_);
            _close(original_fd_);
#endif
        }
    }

    std::string GetOutput()
    {
        if (original_fd_ != -1)
        {
#if PYLABHUB_IS_POSIX
            fflush(stderr);
            dup2(original_fd_, fd_to_capture_);
            close(original_fd_);
#else // Windows
            fflush(stderr);
            _dup2(original_fd_, fd_to_capture_);
            _close(original_fd_);
#endif
            original_fd_ = -1; // Mark as restored
        }

        std::string output;
        std::vector<char> buffer(1024);
        ssize_t bytes_read;
#if PYLABHUB_IS_POSIX
        while ((bytes_read = read(pipe_fds_[0], buffer.data(), buffer.size())) > 0)
        {
            output.append(buffer.data(), static_cast<unsigned int>(bytes_read));
        }
        close(pipe_fds_[0]);
#else // Windows
        while ((bytes_read = _read(pipe_fds_[0], buffer.data(),
                                   static_cast<unsigned int>(buffer.size()))) > 0)
        {
            output.append(buffer.data(), static_cast<unsigned int>(bytes_read));
        }
        _close(pipe_fds_[0]);
#endif
        return output;
    }

  private:
    int fd_to_capture_;
    int original_fd_;
    int pipe_fds_[2];
};

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
size_t count_lines(std::string_view text,
                   std::optional<std::string_view> must_include = std::nullopt,
                   std::optional<std::string_view> must_exclude = std::nullopt);

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
 * This function should be called within a separate process dedicated to running
 * the test logic.
 *
 * @tparam Fn The type of the test logic callable.
 * @tparam Mods The types of the lifecycle modules to manage.
 * @param test_logic The lambda or function containing the test assertions.
 * @param test_name A descriptive name for the test, used in error messages.
 * @param mods The lifecycle modules (e.g., `Logger::GetLifecycleModule()`) required by the test.
 * @return 0 on success, 1 on GTest assertion failure, 2 on standard exception, 3 on unknown
 * exception.
 */
template <typename Fn, typename... Mods>
int run_gtest_worker(Fn test_logic, const char *test_name, Mods &&...mods)
{
    pylabhub::utils::LifecycleGuard guard(
        pylabhub::utils::MakeModDefList(std::forward<Mods>(mods)...));

    try
    {
        test_logic();
    }
    catch (const ::testing::AssertionException &e)
    {
        PLH_DEBUG("[WORKER FAILURE] GTest assertion failed in {}: \n{}", test_name, e.what());
        pylabhub::debug::print_stack_trace();
        return 1;
    }
    catch (const std::exception &e)
    {
        PLH_DEBUG("[WORKER FAILURE] {} threw an exception: {}", test_name, e.what());
        pylabhub::debug::print_stack_trace();
        return 2;
    }
    catch (...)
    {
        PLH_DEBUG("[WORKER FAILURE] {} threw an unknown exception.", test_name);
        pylabhub::debug::print_stack_trace();
        return 3;
    }
    return 0; // Success
}

} // namespace pylabhub::tests::helper