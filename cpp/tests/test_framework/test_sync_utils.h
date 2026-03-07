#pragma once
/**
 * @file test_sync_utils.h
 * @brief Lightweight synchronization primitives for all test layers.
 *
 * This header provides condition-based waiting helpers that replace fixed
 * `sleep_for` calls in tests. Include it directly from any test file —
 * it only depends on `<chrono>` and `<thread>`, no gtest or lifecycle headers.
 *
 * ## Guiding principle
 *
 * Never use `sleep_for` to ORDER concurrent operations in a test. A sleep only
 * tells you time has passed, not that an event has occurred. Under CI load,
 * the scheduler may delay a thread far beyond what a fixed sleep accounts for.
 *
 * Use `poll_until` instead: it blocks on the exact condition that must be true
 * before the next assertion, with a deadline that gives ample CI headroom.
 *
 * Correct pattern:
 * @code
 *   // Wait directly for the expected state, do not sleep and hope.
 *   ASSERT_TRUE(poll_until([&] { return proc.iteration_count() >= 3; }, 3s))
 *       << "Condition not reached within timeout";
 * @endcode
 *
 * @see shared_test_helpers.h — process-level helpers (wait_for_string_in_file, etc.)
 */

#include <chrono>
#include <thread>

namespace pylabhub::tests::helper
{

/**
 * @brief Polls a predicate until it returns true or a deadline is exceeded.
 *
 * Blocks by repeatedly evaluating `pred()` with a `poll_ms` backoff between
 * evaluations. Returns immediately once `pred()` returns true.
 *
 * @param pred     Callable returning bool; true means the condition is met.
 * @param timeout  Maximum time to wait (default: 5 s).
 * @param poll_ms  Sleep duration between polls (default: 10 ms).
 * @return true if pred() became true before the deadline; false on timeout.
 *
 * Usage with GoogleTest:
 * @code
 *   ASSERT_TRUE(poll_until([&] { return q.size() > 0; }, std::chrono::seconds{3}))
 *       << "Queue never received an item";
 * @endcode
 */
template <typename Pred>
bool poll_until(Pred pred,
                std::chrono::milliseconds timeout  = std::chrono::seconds{5},
                std::chrono::milliseconds poll_ms  = std::chrono::milliseconds{10})
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(poll_ms);
    }
    return true;
}

} // namespace pylabhub::tests::helper
