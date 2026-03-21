#pragma once
/**
 * @file role_host_core.hpp
 * @brief RoleHostCore — single source of truth for role state, metrics, and messages.
 *
 * All role hosts, engines, and API classes access state through this class.
 * No raw atomic access outside this class — all reads and writes go through
 * proper methods with correct memory ordering decided in one place.
 *
 * ## Ownership
 * - Created by: role host (member variable)
 * - Passed to: engine via initialize(), API class via constructor
 * - Lifetime: must outlive engine and API class
 *
 * ## Thread safety
 * - Atomic state (shutdown, metrics, errors): safe to read/write from any thread
 * - Message queue: mutex-protected, safe from any thread
 * - Plain state (validate_only, script_load_ok, running): protected by
 *   promise/future synchronization between worker and main thread
 */

#include "utils/script_host_schema.hpp" // SchemaSpec, FieldDef
#include "utils/json_fwd.hpp"           // nlohmann::json (project buffer header)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pylabhub::scripting
{

struct IncomingMessage
{
    std::string              event;
    std::string              sender;
    std::vector<std::byte>   data;
    nlohmann::json           details;
};

class RoleHostCore
{
  public:
    // ── External shutdown flag (from main) ────────────────────────────────
    //
    // g_shutdown is owned by main(). Set ONLY by the signal handler.
    // The role host stores a non-owning pointer and polls it in the loop.
    // Scripts cannot write to it — they use request_stop() instead.

    std::atomic<bool> *g_shutdown{nullptr};

    // ── Message queue ────────────────────────────────────────────────────

    static constexpr size_t kMaxIncomingQueue = 64;

    void enqueue_message(IncomingMessage msg);
    std::vector<IncomingMessage> drain_messages();
    void notify_incoming() noexcept;
    void wait_for_incoming(int timeout_ms) noexcept;

    // ── Plain state (protected by promise/future, not atomics) ───────────

    bool validate_only{false};
    bool script_load_ok{false};
    bool running{false};

    // ── Schema (engine-agnostic) ─────────────────────────────────────────

    SchemaSpec fz_spec;
    size_t     schema_fz_size{0};
    bool       has_fz{false};

    // ── Stop reason enum ─────────────────────────────────────────────────

    enum class StopReason : int
    {
        Normal        = 0,
        PeerDead      = 1,
        HubDead       = 2,
        CriticalError = 3,
    };

    // ── Shutdown / error control ─────────────────────────────────────────
    //
    // All writers use release ordering. The loop condition uses
    // should_exit_loop() which acquires once, then inner checks
    // use relaxed (the acquire at loop top establishes happens-before).

    void request_stop() noexcept
    {
        shutdown_requested_.store(true, std::memory_order_release);
    }

    void set_stop_reason(StopReason r) noexcept
    {
        stop_reason_.store(static_cast<int>(r), std::memory_order_relaxed);
    }

    void set_critical_error() noexcept
    {
        critical_error_.store(true, std::memory_order_release);
        stop_reason_.store(static_cast<int>(StopReason::CriticalError),
                           std::memory_order_relaxed);
        shutdown_requested_.store(true, std::memory_order_release);
    }

    void set_running(bool v) noexcept
    {
        running_threads_.store(v, std::memory_order_release);
    }

    // ── Shutdown / error queries ─────────────────────────────────────────

    [[nodiscard]] bool is_running() const noexcept
    {
        return running_threads_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_shutdown_requested() const noexcept
    {
        return shutdown_requested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_critical_error() const noexcept
    {
        return critical_error_.load(std::memory_order_acquire);
    }

    /// Check all loop exit conditions in one call.
    /// Used as the data loop's while condition.
    [[nodiscard]] bool should_continue_loop() const noexcept
    {
        return running_threads_.load(std::memory_order_acquire) &&
               !shutdown_requested_.load(std::memory_order_acquire) &&
               !critical_error_.load(std::memory_order_relaxed);
    }

    /// Relaxed check for inner retry loops (happens-before established by
    /// should_continue_loop() at the outer loop top).
    [[nodiscard]] bool should_exit_inner() const noexcept
    {
        return !running_threads_.load(std::memory_order_relaxed) ||
               shutdown_requested_.load(std::memory_order_relaxed) ||
               critical_error_.load(std::memory_order_relaxed);
    }

    /// Check if external signal handler requested process exit.
    [[nodiscard]] bool is_process_exit_requested() const noexcept
    {
        return g_shutdown && g_shutdown->load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string stop_reason_string() const noexcept
    {
        switch (stop_reason_.load(std::memory_order_relaxed))
        {
        case 1: return "peer_dead";
        case 2: return "hub_dead";
        case 3: return "critical_error";
        default: return "normal";
        }
    }

    // ── Metric accessors (read) ──────────────────────────────────────────

    [[nodiscard]] uint64_t out_written()       const noexcept { return out_written_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t in_received()       const noexcept { return in_received_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t drops()             const noexcept { return drops_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t script_errors()     const noexcept { return script_errors_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t iteration_count()   const noexcept { return iteration_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t last_cycle_work_us() const noexcept { return last_cycle_work_us_.load(std::memory_order_relaxed); }

    // ── Metric mutators (write) ──────────────────────────────────────────

    void inc_out_written()       noexcept { out_written_.fetch_add(1, std::memory_order_relaxed); }
    void inc_in_received()       noexcept { in_received_.fetch_add(1, std::memory_order_relaxed); }
    void inc_drops()             noexcept { drops_.fetch_add(1, std::memory_order_relaxed); }
    void inc_script_errors()     noexcept { script_errors_.fetch_add(1, std::memory_order_relaxed); }
    void inc_iteration_count()   noexcept { iteration_count_.fetch_add(1, std::memory_order_relaxed); }

    void set_last_cycle_work_us(uint64_t v) noexcept { last_cycle_work_us_.store(v, std::memory_order_relaxed); }

#ifdef PYLABHUB_BUILD_TESTS
    /// Test-only: directly set counter values for test setup.
    /// Production code should use inc_*() only.
    void test_set_out_written(uint64_t v) noexcept { out_written_.store(v, std::memory_order_relaxed); }
    void test_set_in_received(uint64_t v) noexcept { in_received_.store(v, std::memory_order_relaxed); }
    void test_set_drops(uint64_t v)       noexcept { drops_.store(v, std::memory_order_relaxed); }
#endif

  private:
    // ── Shutdown / error state ────────────────────────────────────────────
    std::atomic<bool>     running_threads_{false};
    std::atomic<bool>     shutdown_requested_{false};
    std::atomic<bool>     critical_error_{false};
    std::atomic<int>      stop_reason_{0};

    // ── Metrics ───────────────────────────────────────────────────────────
    std::atomic<uint64_t> script_errors_{0};
    std::atomic<uint64_t> out_written_{0};
    std::atomic<uint64_t> in_received_{0};
    std::atomic<uint64_t> drops_{0};
    std::atomic<uint64_t> iteration_count_{0};
    std::atomic<uint64_t> last_cycle_work_us_{0};

    // ── Message queue ─────────────────────────────────────────────────────
    std::vector<IncomingMessage> incoming_queue_;
    std::mutex                   incoming_mu_;
    std::condition_variable      incoming_cv_;
};

} // namespace pylabhub::scripting
