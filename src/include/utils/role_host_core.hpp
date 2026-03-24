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
 * - Init-time state (script_load_ok, fz_spec, schema_fz_size): set once
 *   during worker_main_ init, then read-only
 */

#include "pylabhub_utils_export.h"
#include "utils/script_host_schema.hpp" // SchemaSpec, FieldDef
#include "utils/json_fwd.hpp"           // nlohmann::json (project buffer header)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pylabhub::hub { class InboxClient; }

namespace pylabhub::scripting
{

struct IncomingMessage
{
    std::string              event;
    std::string              sender;
    std::vector<std::byte>   data;
    nlohmann::json           details;
};

class PYLABHUB_UTILS_EXPORT RoleHostCore
{
  public:
    // ── Message queue ────────────────────────────────────────────────────

    static constexpr size_t kMaxIncomingQueue = 64;

    void enqueue_message(IncomingMessage msg);
    std::vector<IncomingMessage> drain_messages();
    void notify_incoming() noexcept;
    void wait_for_incoming(int timeout_ms) noexcept;

    // ── External shutdown flag (from main) ────────────────────────────────

    void set_shutdown_flag(std::atomic<bool> *flag) noexcept { g_shutdown_ = flag; }

    // ── Plain state (set once during init, read during loop) ─────────────

    void set_validate_only(bool v) noexcept { validate_only_ = v; }
    [[nodiscard]] bool is_validate_only() const noexcept { return validate_only_; }

    void set_script_load_ok(bool v) noexcept
    {
        script_load_ok_.store(v, std::memory_order_release);
    }
    [[nodiscard]] bool is_script_load_ok() const noexcept
    {
        return script_load_ok_.load(std::memory_order_acquire);
    }

    // ── Schema (engine-agnostic, set once during init) ───────────────────

    void set_fz_spec(SchemaSpec spec, size_t fz_size) noexcept
    {
        fz_spec_        = std::move(spec);
        schema_fz_size_ = fz_size;
        has_fz_         = fz_spec_.has_schema;
    }
    [[nodiscard]] const SchemaSpec &fz_spec()        const noexcept { return fz_spec_; }
    [[nodiscard]] size_t            schema_fz_size() const noexcept { return schema_fz_size_; }
    [[nodiscard]] bool              has_fz()         const noexcept { return has_fz_; }

    // ── Inbox cache (role-level, shared across engine states) ────────────

    struct InboxCacheEntry
    {
        std::shared_ptr<hub::InboxClient> client;
        std::string type_name;     ///< FFI/ctypes type name for the inbox slot.
        size_t      item_size{0};  ///< Size of one inbox slot in bytes.
    };

    /// Get a cached inbox client for the given target UID, or nullptr.
    std::shared_ptr<hub::InboxClient> get_inbox_client(const std::string &target_uid) const;

    /// Get the full cache entry (client + type info), or nullopt if not cached.
    std::optional<InboxCacheEntry> get_inbox_entry(const std::string &target_uid) const;

    /// Store an inbox client in the cache.
    void set_inbox_entry(const std::string &target_uid, InboxCacheEntry entry);

    /// Stop and remove all cached inbox clients.
    void clear_inbox_cache();

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
        return g_shutdown_ && g_shutdown_->load(std::memory_order_relaxed);
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

    // ── External shutdown flag ───────────────────────────────────────────
    std::atomic<bool> *g_shutdown_{nullptr};

    // ── Init-time state (set once, read during loop) ─────────────────────
    bool              validate_only_{false};
    std::atomic<bool> script_load_ok_{false};
    SchemaSpec fz_spec_;
    size_t     schema_fz_size_{0};
    bool       has_fz_{false};

    // ── Inbox cache ──────────────────────────────────────────────────────
    mutable std::unordered_map<std::string, InboxCacheEntry> inbox_cache_;
    mutable std::mutex inbox_cache_mu_;
};

} // namespace pylabhub::scripting
