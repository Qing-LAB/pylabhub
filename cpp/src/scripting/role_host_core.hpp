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
    std::string            sender; ///< ZMQ identity of sender (empty for consumer data msgs)
    std::vector<std::byte> data;   ///< Raw payload bytes
};

// ============================================================================
// RoleHostCore
// ============================================================================

class RoleHostCore
{
  public:
    static constexpr std::size_t kMaxIncomingQueue{64};

    // ── Message queue ────────────────────────────────────────────────────────

    /** Thread-safe enqueue; drops if at capacity. */
    void enqueue_message(IncomingMessage msg);

    /** Drain all queued messages (returns empty vector if none). */
    std::vector<IncomingMessage> drain_messages();

    /** Wake any thread blocked on the condition variable. */
    void notify_incoming();

    // ── Shutdown coordination ────────────────────────────────────────────────

    std::atomic<bool> *g_shutdown{nullptr};        ///< External shutdown flag (from main)
    std::atomic<bool>  shutdown_requested{false};   ///< Internal shutdown (from API stop())

    // ── Worker thread lifecycle ──────────────────────────────────────────────

    std::atomic<bool> running_threads{false};

    // ── State flags ──────────────────────────────────────────────────────────

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
