#pragma once
/**
 * @file producer_role_host.hpp
 * @brief Unified producer role host — engine-agnostic.
 *
 * ProducerRoleHost owns:
 * - Layer 3: Infrastructure (Messenger, Producer, queue, ctrl_thread_, events)
 * - Layer 2: Data loop (inner retry acquire, deadline wait, invoke, commit)
 *
 * The script engine (Layer 1) is injected as a ScriptEngine pointer.
 * All script operations happen on the worker thread.
 *
 * See docs/tech_draft/loop_design_unified.md for the full design.
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
class Producer;
class QueueWriter;
class InboxQueue;
} // namespace pylabhub::hub

namespace pylabhub::producer
{

class ProducerRoleHost
{
  public:
    explicit ProducerRoleHost(config::RoleConfig config,
                              std::unique_ptr<scripting::ScriptEngine> engine,
                              std::atomic<bool> *shutdown_flag = nullptr);
    ~ProducerRoleHost();

    // Non-copyable, non-movable (owns threads).
    ProducerRoleHost(const ProducerRoleHost &) = delete;
    ProducerRoleHost &operator=(const ProducerRoleHost &) = delete;
    ProducerRoleHost(ProducerRoleHost &&) = delete;
    ProducerRoleHost &operator=(ProducerRoleHost &&) = delete;

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

    bool setup_infrastructure_();
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
    hub::Messenger                         out_messenger_;
    std::optional<hub::Producer>           out_producer_;
    std::unique_ptr<hub::QueueWriter>      queue_;
    std::unique_ptr<hub::InboxQueue>       inbox_queue_;
    std::thread                            ctrl_thread_;

    // Schema info (resolved from config during setup).
    // fz_spec is stored in core_.fz_spec() (shared with engine for flexzone exposure).
    scripting::SchemaSpec                  slot_spec_;
    size_t                                 schema_slot_size_{0};
    std::string                            inbox_type_name_;

    // Metrics are in core_ (RoleHostCore) — single source of truth.
    // See core_.out_written_, core_.drops_, core_.iteration_count_, core_.last_cycle_work_us_.
};

} // namespace pylabhub::producer
