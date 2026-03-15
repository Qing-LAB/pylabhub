#pragma once
/**
 * @file role_host_core.hpp
 * @brief RoleHostCore — engine-agnostic infrastructure for script host roles.
 *
 * Pure C++ infrastructure shared by all scripting engines (Python, Lua, etc.)
 * and all roles (producer, consumer, processor).  No pybind11, no Lua headers.
 *
 * Provides:
 *  - Thread-safe bounded incoming message queue (ZMQ → handler path)
 *  - Condvar-based wakeup for the monitoring loop (notify_incoming / wait_for_incoming)
 *  - Shutdown coordination flags
 *  - Worker thread lifecycle flag
 *  - State flags (validate_only, script_load_ok, running)
 *  - FlexZone schema storage (engine-agnostic SchemaSpec)
 *
 * Composed (not inherited) by PythonRoleHostBase and future LuaRoleHostBase.
 *
 * See HEP-CORE-0011 for the ScriptHost abstraction framework.
 */

#include "utils/script_host_schema.hpp"

#include "utils/json_fwd.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// IncomingMessage — ZMQ message queued for the script handler
// ============================================================================

struct IncomingMessage
{
    std::string            sender;  ///< ZMQ identity of sender (empty for consumer data msgs)
    std::vector<std::byte> data;    ///< Raw payload bytes
    std::string            event;   ///< Non-empty → event message (not data)
    nlohmann::json         details; ///< Event payload (for events only)
};

// ============================================================================
// RoleHostCore
// ============================================================================

class RoleHostCore
{
  public:
    static constexpr std::size_t kMaxIncomingQueue{64};

    // ── Message queue ────────────────────────────────────────────────────────

    /** Thread-safe enqueue; drops if at capacity. Notifies any wait_for_incoming() waiter. */
    void enqueue_message(IncomingMessage msg);

    /** Drain all queued messages (returns empty vector if none). */
    std::vector<IncomingMessage> drain_messages();

    /**
     * @brief Wake all wait_for_incoming() waiters immediately.
     *
     * Called from stop_role() so the monitoring loop (run_role_main_loop) exits
     * without waiting the full kAdminPollIntervalMs sleep.
     */
    void notify_incoming() noexcept;

    /**
     * @brief Block until notified or timeout_ms elapses.
     *
     * Used by the monitoring loop (run_role_main_loop) instead of sleep_for so
     * that stop_role() can unblock it instantly via notify_incoming().
     */
    void wait_for_incoming(int timeout_ms) noexcept;

    // ── Shutdown coordination ────────────────────────────────────────────────
    //
    // Two separate shutdown paths by design:
    //   g_shutdown        — EXTERNAL: pointer to the process-level flag set by
    //                       the signal handler in main(). All role threads poll
    //                       this flag to react to SIGINT/SIGTERM. The flag is
    //                       owned by main(); we hold only a non-owning pointer.
    //   shutdown_requested — INTERNAL: set by Python API stop() or by the role
    //                        itself (e.g., set_critical_error). Independent of
    //                        the external signal — allows graceful self-stop
    //                        without affecting the whole process.
    //
    // Both are checked in the role main loop; either one triggers shutdown.

    std::atomic<bool> *g_shutdown{nullptr};        ///< External shutdown flag (from main)
    std::atomic<bool>  shutdown_requested{false};   ///< Internal shutdown (from API stop())

    // ── Worker thread lifecycle ──────────────────────────────────────────────

    std::atomic<bool> running_threads{false};

    // ── State flags ──────────────────────────────────────────────────────────
    //
    // These plain bools are written by the interpreter thread BEFORE signal_ready_()
    // (which does memory_order_release), and read by the main thread AFTER future.get()
    // (which provides the corresponding acquire). The release-acquire chain through
    // the promise/future makes these reads safe without extra atomics.

    bool validate_only{false};
    bool script_load_ok{false};
    bool running{false};

    // ── FlexZone schema (engine-agnostic) ────────────────────────────────────

    SchemaSpec fz_spec;
    size_t     schema_fz_size{0};
    bool       has_fz{false};

  private:
    std::vector<IncomingMessage> incoming_queue_;
    std::mutex                   incoming_mu_;
    std::condition_variable      incoming_cv_;
};

} // namespace pylabhub::scripting
