#pragma once
/**
 * @file processor_role_host.hpp
 * @brief Unified processor role host — engine-agnostic.
 *
 * ProcessorRoleHost owns:
 * - Layer 3: Infrastructure (dual Messengers, Consumer + Producer, dual queues,
 *            ctrl_thread_, events)
 * - Layer 2: Data loop (dual-queue inner retry acquire, deadline wait, invoke,
 *            commit/release)
 *
 * The script engine (Layer 1) is injected as a ScriptEngine pointer.
 * All script operations happen on the worker thread.
 *
 * See docs/tech_draft/loop_design_unified.md §5 for the full design.
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
class Producer;
class InboxQueue;
} // namespace pylabhub::hub

namespace pylabhub::processor
{

class ProcessorRoleHost
{
  public:
    explicit ProcessorRoleHost(config::RoleConfig config,
                               std::unique_ptr<scripting::ScriptEngine> engine,
                               std::atomic<bool> *shutdown_flag = nullptr);
    ~ProcessorRoleHost();

    // Non-copyable, non-movable (owns threads).
    ProcessorRoleHost(const ProcessorRoleHost &) = delete;
    ProcessorRoleHost &operator=(const ProcessorRoleHost &) = delete;
    ProcessorRoleHost(ProcessorRoleHost &&) = delete;
    ProcessorRoleHost &operator=(ProcessorRoleHost &&) = delete;

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

    scripting::RoleHostCore                  core_;
    config::RoleConfig                       config_;
    std::unique_ptr<scripting::ScriptEngine> engine_;

    // Worker thread.
    std::thread                              worker_thread_;
    std::promise<bool>                       ready_promise_;

    // Infrastructure (created on worker thread in setup_infrastructure_).
    hub::Messenger                           in_messenger_;
    hub::Messenger                           out_messenger_;
    std::optional<hub::Consumer>             in_consumer_;
    std::optional<hub::Producer>             out_producer_;


    std::unique_ptr<hub::InboxQueue>         inbox_queue_;
    std::thread                              ctrl_thread_;

    // Schema info (resolved from config during setup).
    scripting::SchemaSpec                    in_slot_spec_;
    scripting::SchemaSpec                    out_slot_spec_;
    size_t                                   in_schema_slot_size_{0};
    size_t                                   out_schema_slot_size_{0};
};

} // namespace pylabhub::processor
