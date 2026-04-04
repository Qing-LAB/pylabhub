#pragma once
/**
 * @file consumer_role_host.hpp
 * @brief Unified consumer role host — engine-agnostic.
 *
 * ConsumerRoleHost owns:
 * - Layer 3: Infrastructure (Messenger, Consumer, queue, ctrl_thread_, events)
 * - Layer 2: Data loop (inner retry acquire, read, invoke, release)
 *
 * The script engine (Layer 1) is injected as a ScriptEngine pointer.
 * All script operations happen on the worker thread.
 *
 * See docs/tech_draft/loop_design_unified.md §4 for the full design.
 */

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
class Consumer;
class InboxQueue;
} // namespace pylabhub::hub

namespace pylabhub::consumer
{

class ConsumerRoleHost
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
    void run_ctrl_thread_();
    nlohmann::json snapshot_metrics_json() const;

    // ── Data loop (Layer 2) ──────────────────────────────────────────────────

    void run_data_loop_();
    void drain_inbox_sync_();

    // ── Members ──────────────────────────────────────────────────────────────

    scripting::RoleHostCore                core_;
    config::RoleConfig                     config_;
    std::unique_ptr<scripting::ScriptEngine> engine_;

    // Worker thread.
    std::thread                            worker_thread_;
    std::promise<bool>                     ready_promise_;

    // Infrastructure (created on worker thread in setup_infrastructure_).
    hub::Messenger                         in_messenger_;
    std::optional<hub::Consumer>           in_consumer_;

    std::unique_ptr<hub::InboxQueue>       inbox_queue_;
    std::thread                            ctrl_thread_;

    // Role API (created on worker thread, passed to engine).
    std::unique_ptr<scripting::RoleAPIBase> api_;

    // Schema info (resolved from config during setup).
    hub::SchemaSpec                  in_slot_spec_;

    // Metrics are in core_ (RoleHostCore) — single source of truth.
    // See core_.in_received_, core_.iteration_count_, core_.last_cycle_work_us_.
    std::atomic<uint64_t>                  last_seq_{0}; // consumer-specific, not in core
};

} // namespace pylabhub::consumer
