#pragma once
/**
 * @file consumer_role_host.hpp
 * @brief Unified consumer role host — engine-agnostic.
 *
 * ConsumerRoleHost owns:
 * - Layer 3: Infrastructure (BrokerRequestComm, Consumer, queue, ctrl_thread_, events)
 * - Layer 2: Data loop (inner retry acquire, read, invoke, release)
 *
 * The script engine (Layer 1) is injected as a ScriptEngine pointer.
 * All script operations happen on the worker thread.
 *
 * See docs/tech_draft/loop_design_unified.md §4 for the full design.
 */

#include "pylabhub_utils_export.h"
#include "utils/config/role_config.hpp"
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"
#include "plh_datahub.hpp"

#include <atomic>
#include <memory>
#include <future>
#include <optional>
#include <thread>

namespace pylabhub::hub
{
class InboxQueue;
class BrokerRequestComm;
} // namespace pylabhub::hub

namespace pylabhub::consumer
{

class PYLABHUB_UTILS_EXPORT ConsumerRoleHost
{
  public:
    explicit ConsumerRoleHost(config::RoleConfig config,
                              std::unique_ptr<scripting::ScriptEngine> engine,
                              std::atomic<bool> *shutdown_flag = nullptr);
    ~ConsumerRoleHost();

    // Non-copyable, non-movable (owns threads).
    ConsumerRoleHost(const ConsumerRoleHost &) = delete;
    ConsumerRoleHost &operator=(const ConsumerRoleHost &) = delete;
    ConsumerRoleHost(ConsumerRoleHost &&) = delete;
    ConsumerRoleHost &operator=(ConsumerRoleHost &&) = delete;

    // ── Configuration ────────────────────────────────────────────────────────

    void set_validate_only(bool v) { core_.set_validate_only(v); }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /// Spawn worker thread, block until ready (or failure).
    void startup_();

    /// Signal shutdown, join worker thread.
    void shutdown_();

    // ── Queries (called from main thread) ────────────────────────────────────

    [[nodiscard]] bool is_running() const { return core_.is_running(); }
    [[nodiscard]] bool script_load_ok() const { return core_.is_script_load_ok(); }
    [[nodiscard]] const config::RoleConfig &config() const { return config_; }

    /// Block until wakeup (shutdown, incoming message, or timeout).
    void wait_for_wakeup(int timeout_ms) { core_.wait_for_incoming(timeout_ms); }

  private:
    // ── Worker thread entry point ────────────────────────────────────────────

    void worker_main_();

    // ── Infrastructure setup/teardown (Layer 3) ──────────────────────────────

    bool setup_infrastructure_(const hub::SchemaSpec &inbox_spec);
    void teardown_infrastructure_();

    // ── Members ──────────────────────────────────────────────────────────────

    scripting::RoleHostCore                core_;
    config::RoleConfig                     config_;
    std::unique_ptr<scripting::ScriptEngine> engine_;

    // Worker thread — spawned through api_->thread_manager().spawn("worker", ...)
    // so teardown is bounded + ERROR-logged + detaches-on-timeout like every
    // other role-scope thread. The old raw std::thread + unbounded .join()
    // pattern is gone.
    std::promise<bool>                     ready_promise_;

    // Infrastructure (created on worker thread in setup_infrastructure_).
    std::unique_ptr<hub::BrokerRequestComm> broker_comm_;
    std::unique_ptr<hub::InboxQueue>       inbox_queue_;
    config::InboxConfig                    inbox_cfg_;

    // Role API (created on worker thread, passed to engine).
    std::unique_ptr<scripting::RoleAPIBase> api_;

    // Schema info (resolved from config during setup).
    hub::SchemaSpec                  in_slot_spec_;

    // Lifecycle module name (for UnloadModule on shutdown).
    std::string                      engine_module_name_;

    // last_seq: read directly from Consumer::last_seq() → QueueReader (single source of truth).
};

} // namespace pylabhub::consumer
