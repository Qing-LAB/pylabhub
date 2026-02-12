// tests/test_harness/shared_test_helpers.h
#pragma once

// Must be first: defines PYLABHUB_IS_POSIX before any platform-conditional includes.
#include "plh_platform.hpp"

#include <filesystem>
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

#include "gtest/gtest.h"

// Required for run_gtest_worker: LifecycleGuard, PLH_DEBUG, print_stack_trace (Layer 2 umbrella)
#include "plh_service.hpp"

// Required for ThreadRacer
#include <algorithm>
#include <atomic>
#include <exception>
#include <thread>

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
    // Enable throw-on-failure so ASSERT_* and EXPECT_* throw on failure.
    // Without this, ASSERT_* only returns from the lambda (not throws), and
    // EXPECT_* prints to stderr but continues — both causing silent false-passes.
    // GTest throws ::testing::internal::GoogleTestFailureException (base class of
    // AssertionException) when throw_on_failure is set.
    GTEST_FLAG_SET(throw_on_failure, true);

    pylabhub::utils::LifecycleGuard guard(
        pylabhub::utils::MakeModDefList(std::forward<Mods>(mods)...));

    try
    {
        test_logic();
    }
    catch (const ::testing::internal::GoogleTestFailureException &e)
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

/**
 * @brief Wraps worker logic with NO lifecycle initialization.
 *
 * Use this when the worker itself needs to control lifecycle steps —
 * e.g., testing pre-init state, staged initialization, or partial module loads.
 * The test_logic lambda is responsible for calling LifecycleGuard / InitializeApp
 * / FinalizeApp as needed.
 *
 * @param test_logic The lambda containing test assertions and manual lifecycle calls.
 * @param test_name  Descriptive name for error messages.
 * @return 0 on success, 1 GTest failure, 2 std::exception, 3 unknown exception.
 */
template <typename Fn> int run_worker_bare(Fn test_logic, const char *test_name)
{
    // Same throw_on_failure fix as run_gtest_worker: makes ASSERT_*/EXPECT_* throw.
    GTEST_FLAG_SET(throw_on_failure, true);

    try
    {
        test_logic();
    }
    catch (const ::testing::internal::GoogleTestFailureException &e)
    {
        PLH_DEBUG("[WORKER BARE FAILURE] GTest assertion failed in {}: \n{}", test_name, e.what());
        pylabhub::debug::print_stack_trace();
        return 1;
    }
    catch (const std::exception &e)
    {
        PLH_DEBUG("[WORKER BARE FAILURE] {} threw an exception: {}", test_name, e.what());
        pylabhub::debug::print_stack_trace();
        return 2;
    }
    catch (...)
    {
        PLH_DEBUG("[WORKER BARE FAILURE] {} threw an unknown exception.", test_name);
        pylabhub::debug::print_stack_trace();
        return 3;
    }
    return 0;
}

// ============================================================================
// ThreadRacer — concurrent test execution inside worker processes
// ============================================================================

/**
 * @brief Runs N threads simultaneously to test concurrent behavior.
 *
 * All threads start at the same time (synchronized via a barrier).
 * Any exception thrown by a thread is captured and re-thrown from race().
 *
 * Usage (inside a worker function):
 * @code
 *   ThreadRacer racer(8);
 *   bool ok = racer.race([&](int thread_id) {
 *       FileLock lock(path, ResourceType::File, LockMode::NonBlocking);
 *       // ... assertions ...
 *   });
 *   if (!ok) {
 *       for (auto& eptr : racer.exceptions()) {
 *           try { std::rethrow_exception(eptr); }
 *           catch (const std::exception& e) { PLH_DEBUG("Thread failed: {}", e.what()); }
 *       }
 *       return 1;
 *   }
 * @endcode
 */
class ThreadRacer
{
  public:
    explicit ThreadRacer(int n_threads) : n_threads_(n_threads) {}

    /**
     * @brief Runs fn(thread_index) on n_threads simultaneously.
     *
     * All threads synchronize on a barrier before starting work, maximizing
     * the chance of true concurrency and exposing race conditions.
     *
     * @return true if all threads completed without throwing, false otherwise.
     */
    template <typename F> bool race(F fn)
    {
        exceptions_.clear();
        exceptions_.resize(static_cast<size_t>(n_threads_));

        std::atomic<int> ready_count{0};
        std::atomic<bool> start_flag{false};

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(n_threads_));

        for (int i = 0; i < n_threads_; ++i)
        {
            threads.emplace_back(
                [&, i]()
                {
                    ready_count.fetch_add(1, std::memory_order_release);
                    // Spin until all threads are ready
                    while (!start_flag.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    try
                    {
                        fn(i);
                    }
                    catch (...)
                    {
                        exceptions_[static_cast<size_t>(i)] = std::current_exception();
                    }
                });
        }

        // Wait until all threads are at the barrier
        while (ready_count.load(std::memory_order_acquire) < n_threads_)
            std::this_thread::yield();

        // Release all threads simultaneously
        start_flag.store(true, std::memory_order_release);

        for (auto &t : threads)
            t.join();

        return std::all_of(exceptions_.begin(), exceptions_.end(),
                           [](const std::exception_ptr &p) { return p == nullptr; });
    }

    const std::vector<std::exception_ptr> &exceptions() const { return exceptions_; }

  private:
    int n_threads_;
    std::vector<std::exception_ptr> exceptions_;
};

// ============================================================================
// Process Ready Signal (for deterministic parent-child init ordering)
// ============================================================================

/**
 * @brief Signals "ready" to the parent when PLH_TEST_READY_FD (POSIX) or
 * PLH_TEST_READY_HANDLE (Windows) is set. No-op if not set. Call from worker
 * after init is complete; parent blocks on wait_for_ready() until then.
 */
void signal_test_ready();

// ============================================================================
// DataBlock Test Utilities (for layered test architecture)
// ============================================================================

/**
 * @brief Generates unique test channel name with timestamp.
 * @param test_name Base name (e.g., "SchemaValidation")
 * @return Unique channel name (e.g., "test_SchemaValidation_1675960234567")
 */
std::string make_test_channel_name(const char *test_name);

/**
 * @brief Cleans up shared memory DataBlock after test.
 * @param channel_name Channel name to clean up
 * @return True if cleanup succeeded
 */
bool cleanup_test_datablock(const std::string &channel_name);

/**
 * @brief RAII guard for test DataBlock cleanup.
 * @details Automatically generates unique channel name and cleans up on destruction.
 *
 * Usage:
 * @code
 *   DataBlockTestGuard guard("MyTest");
 *   auto producer = create_datablock_producer(hub, guard.channel_name(), ...);
 *   // ~DataBlockTestGuard() automatically cleans up
 * @endcode
 */
class DataBlockTestGuard
{
  public:
    explicit DataBlockTestGuard(const char *test_name)
        : channel_name_(make_test_channel_name(test_name))
    {
    }

    ~DataBlockTestGuard() { cleanup_test_datablock(channel_name_); }

    const std::string &channel_name() const { return channel_name_; }

    DataBlockTestGuard(const DataBlockTestGuard &) = delete;
    DataBlockTestGuard &operator=(const DataBlockTestGuard &) = delete;
    DataBlockTestGuard(DataBlockTestGuard &&) = delete;
    DataBlockTestGuard &operator=(DataBlockTestGuard &&) = delete;

  private:
    std::string channel_name_;
};

} // namespace pylabhub::tests::helper