#pragma once
/**
 * @file backoff_strategy.hpp
 * @brief Header-only backoff strategies for busy-wait loops.
 *
 * This module provides configurable backoff strategies to reduce CPU contention
 * and power consumption in spin loops (busy-wait scenarios). Backoff is critical
 * for performance in scenarios where a thread repeatedly attempts to acquire a
 * resource (lock, slot, connection) that may not be immediately available.
 *
 * Design Philosophy:
 * - Policy-based: Different strategies for different scenarios
 * - Header-only: Zero overhead, easy to inline
 * - Template-based: Compile-time strategy selection
 * - Testable: Can inject NoBackoff for fast unit tests
 *
 * Usage Scenarios:
 * - SharedSpinLock: ExponentialBackoff (contention rare, yield quickly)
 * - SlotRWState: ExponentialBackoff (high throughput, adaptive)
 * - FileLock: ExponentialBackoff (I/O latency varies)
 * - MessageHub: ExponentialBackoff (network reconnect)
 * - Unit Tests: NoBackoff (fast test execution)
 *
 * @see HEP-CORE-0002-DataHub-FINAL.md Section 4.2 (SlotRWState coordination)
 */
#include <chrono>
#include <thread>

namespace pylabhub::utils
{

// ============================================================================
// Backoff Strategies
// ============================================================================

/**
 * @brief Exponential backoff strategy with three phases.
 * @details Optimized for scenarios where contention is typically short-lived
 *          but may occasionally persist.
 *
 * Phase 1 (iterations 0-3): yield() - cooperative multitasking, minimal overhead
 * Phase 2 (iterations 4-9): 1us sleep - transition to light sleep
 * Phase 3 (iterations 10+): exponential sleep - reduce bus traffic
 *
 * Total backoff time at iteration N:
 * - N=0-3:  ~0us (just yield)
 * - N=4-9:  ~1us per iteration = 6us total
 * - N=10:   10us
 * - N=20:   200us
 * - N=50:   500us
 * - N=100:  1000us = 1ms
 *
 * Use Cases:
 * - SharedSpinLock (cross-process, PID-based)
 * - SlotRWState (writer/reader acquisition)
 * - FileLock (POSIX advisory locks)
 *
 * @example
 * ExponentialBackoff backoff;
 * int iteration = 0;
 * while (!lock.try_acquire()) {
 *     backoff(iteration++);
 *     if (iteration > 1000) { timeout(); break; }
 * }
 */
struct ExponentialBackoff
{
    /**
     * @brief Performs backoff based on iteration count.
     * @param iteration The current iteration number (0-based).
     */
    void operator()(int iteration) const noexcept
    {
        if (iteration < 4)
        {
            // Phase 1: Fast path - just yield to other threads
            // Typical latency: 0-10us depending on scheduler
            std::this_thread::yield();
        }
        else if (iteration < 10)
        {
            // Phase 2: Light sleep - reduce CPU usage but stay responsive
            // Typical latency: 1-100us depending on OS timer resolution
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        else
        {
            // Phase 3: Exponential backoff - reduce memory bus contention
            // Typical latency: 10us - 1ms (grows linearly with iteration)
            std::this_thread::sleep_for(
                std::chrono::microseconds(static_cast<long>(iteration * 10)));
        }
    }
};

/**
 * @brief Constant backoff strategy with fixed delay.
 * @details Always sleeps for a fixed duration regardless of iteration count.
 *          Useful for scenarios with predictable contention patterns.
 *
 * Advantages:
 * - Predictable latency (good for real-time systems)
 * - Simple to reason about
 * - No exponential explosion
 *
 * Disadvantages:
 * - May be too aggressive (wastes time if resource becomes available quickly)
 * - May be too conservative (wastes CPU if delay is too short)
 *
 * Use Cases:
 * - Testing with controlled timing
 * - Real-time systems with strict latency requirements
 * - Scenarios where contention is uniformly distributed
 *
 * @example
 * ConstantBackoff backoff(100us); // Always wait 100us
 * int iteration = 0;
 * while (!resource.is_ready()) {
 *     backoff(iteration++);
 * }
 */
struct ConstantBackoff
{
    std::chrono::microseconds delay;

    /**
     * @brief Constructs a ConstantBackoff with specified delay.
     * @param d The fixed delay to use on each backoff (default: 100us).
     */
    explicit ConstantBackoff(std::chrono::microseconds d = std::chrono::microseconds(100))
        : delay(d)
    {
    }

    /**
     * @brief Performs backoff with fixed delay.
     * @param iteration Ignored (delay is constant).
     */
    void operator()(int iteration) const noexcept
    {
        (void)iteration; // Unused
        std::this_thread::sleep_for(delay);
    }
};

/**
 * @brief No-op backoff strategy (does nothing).
 * @details Useful for unit testing where you want to spin without delays
 *          to maximize test execution speed. Also useful when backoff is
 *          handled externally (e.g., by a condition variable).
 *
 * WARNING: Using NoBackoff in production can cause:
 * - 100% CPU usage (busy-wait)
 * - Memory bus saturation (constant CAS operations)
 * - Power consumption spikes
 * - Performance degradation on other cores
 *
 * Use Cases:
 * - Unit tests (fast execution, deterministic timing)
 * - Benchmarks (measure raw lock performance)
 * - When backoff is handled externally
 *
 * @example
 * #ifdef UNIT_TEST
 *     using BackoffStrategy = NoBackoff;
 * #else
 *     using BackoffStrategy = ExponentialBackoff;
 * #endif
 */
struct NoBackoff
{
    /**
     * @brief No-op backoff (does nothing).
     * @param iteration Ignored.
     */
    void operator()(int iteration) const noexcept
    {
        (void)iteration; // Unused
        // Intentionally empty - no backoff
    }
};

/**
 * @brief Aggressive exponential backoff for long-wait scenarios.
 * @details Similar to ExponentialBackoff but with faster exponential growth.
 *          Suitable for scenarios where waiting is expected to be long
 *          (e.g., network reconnection, I/O retry).
 *
 * Phase 1 (iterations 0-1): yield()
 * Phase 2 (iterations 2-5): 10us sleep
 * Phase 3 (iterations 6+): exponential sleep (iteration^2 * 10us)
 *
 * Total backoff time at iteration N:
 * - N=0-1:  ~0us (just yield)
 * - N=2-5:  10us per iteration = 40us total
 * - N=6:    360us
 * - N=10:   1000us = 1ms
 * - N=20:   4000us = 4ms
 * - N=50:   25000us = 25ms
 *
 * Use Cases:
 * - Network reconnection (MessageHub)
 * - Slow I/O operations (FileLock on network filesystem)
 * - Cross-machine synchronization
 *
 * @example
 * AggressiveBackoff backoff;
 * int iteration = 0;
 * while (!network.reconnect()) {
 *     backoff(iteration++);
 *     if (iteration > 100) { give_up(); break; }
 * }
 */
struct AggressiveBackoff
{
    /**
     * @brief Performs aggressive backoff with quadratic growth.
     * @param iteration The current iteration number (0-based).
     */
    void operator()(int iteration) const noexcept
    {
        if (iteration < 2)
        {
            std::this_thread::yield();
        }
        else if (iteration < 6)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        else
        {
            // Quadratic growth: iteration^2 * 10us
            // Capped at 100ms to prevent excessive delays
            long delay_us = static_cast<long>(iteration) * static_cast<long>(iteration) * 10;
            if (delay_us > 100000) // Cap at 100ms
            {
                delay_us = 100000;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        }
    }
};

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Convenience function for simple exponential backoff.
 * @details Non-template version for cases where you don't want to specify
 *          the strategy type explicitly.
 *
 * @param iteration The current iteration count.
 *
 * @example
 * int iteration = 0;
 * while (!condition) {
 *     backoff(iteration++);
 * }
 */
inline void backoff(int iteration) noexcept
{
    ExponentialBackoff{}(iteration);
}

} // namespace pylabhub::utils
