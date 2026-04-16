/**
 * @file data_loop.cpp
 * @brief Implementation of retry_acquire (non-template, compiled once).
 */
#include "service/data_loop.hpp"

namespace pylabhub::scripting
{

// ============================================================================
// retry_acquire
// ============================================================================

void *retry_acquire(
    const AcquireContext &ctx,
    RoleHostCore &core,
    const std::function<void *(std::chrono::milliseconds)> &try_once)
{
    // MaxRate: single non-blocking attempt, no retry, no metrics.
    if (ctx.is_max_rate)
        return try_once(std::chrono::milliseconds{0});

    // Timed policies: retry until success, shutdown, or deadline exhaustion.
    while (true)
    {
        void *ptr = try_once(ctx.short_timeout);
        if (ptr != nullptr)
            return ptr;

        core.inc_acquire_retry();

        if (!core.is_running() ||
            core.is_shutdown_requested() ||
            core.is_critical_error())
        {
            return nullptr;
        }
        if (core.is_process_exit_requested())
            return nullptr;

        // First cycle (deadline == max): retry indefinitely until success or shutdown.
        if (ctx.deadline != std::chrono::steady_clock::time_point::max())
        {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    ctx.deadline - std::chrono::steady_clock::now());
            if (remaining <= ctx.short_timeout_us)
                return nullptr; // not enough time for another attempt
        }
    }
}

} // namespace pylabhub::scripting
