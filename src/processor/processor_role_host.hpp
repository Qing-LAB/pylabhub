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

#include "processor_config.hpp"
#include "role_host_core.hpp"
#include "script_engine.hpp"
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
class QueueReader;
class QueueWriter;
class InboxQueue;
} // namespace pylabhub::hub

namespace pylabhub::processor
{

class ProcessorRoleHost
{
  public:
    ProcessorRoleHost() = default;
    ~ProcessorRoleHost();

    // Non-copyable, non-movable (owns threads).
    ProcessorRoleHost(const ProcessorRoleHost &) = delete;
    ProcessorRoleHost &operator=(const ProcessorRoleHost &) = delete;
    ProcessorRoleHost(ProcessorRoleHost &&) = delete;
    ProcessorRoleHost &operator=(ProcessorRoleHost &&) = delete;

    // ── Configuration (call before startup_) ────────────────────────────────

    void set_engine(std::unique_ptr<scripting::ScriptEngine> engine);
    void set_config(ProcessorConfig config);
    void set_validate_only(bool v) { validate_only_ = v; }
    void set_shutdown_flag(std::atomic<bool> *flag) { core_.g_shutdown = flag; }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /// Spawn worker thread, block until ready (or failure).
    void startup_();

    /// Signal shutdown, join worker thread.
    void shutdown_();

    // ── Queries (called from main thread) ────────────────────────────────────

    [[nodiscard]] bool is_running() const { return core_.running_threads.load(std::memory_order_acquire); }
    [[nodiscard]] bool script_load_ok() const { return script_load_ok_.load(std::memory_order_acquire); }
    [[nodiscard]] const ProcessorConfig &config() const { return config_; }

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
    ProcessorConfig                          config_;
    std::unique_ptr<scripting::ScriptEngine> engine_;
    bool                                     validate_only_{false};
    std::atomic<bool>                        script_load_ok_{false};

    // Worker thread.
    std::thread                              worker_thread_;
    std::promise<bool>                       ready_promise_;

    // Infrastructure (created on worker thread in setup_infrastructure_).
    hub::Messenger                           in_messenger_;
    hub::Messenger                           out_messenger_;
    std::optional<hub::Consumer>             in_consumer_;
    std::optional<hub::Producer>             out_producer_;

    // Owned queue storage (non-null only for SHM or direct-ZMQ transport).
    std::unique_ptr<hub::QueueReader>        in_queue_;
    std::unique_ptr<hub::QueueWriter>        out_queue_;
    // Raw pointers: point to either owned queue or broker-ZMQ queue from Consumer/Producer.
    hub::QueueReader                        *in_q_{nullptr};
    hub::QueueWriter                        *out_q_{nullptr};

    std::unique_ptr<hub::InboxQueue>         inbox_queue_;
    std::thread                              ctrl_thread_;

    // Schema info (resolved from config during setup).
    // fz_spec is stored in core_.fz_spec (shared with engine for flexzone exposure).
    scripting::SchemaSpec                    in_slot_spec_;
    scripting::SchemaSpec                    out_slot_spec_;
    size_t                                   in_schema_slot_size_{0};
    size_t                                   out_schema_slot_size_{0};
    std::string                              inbox_type_name_;

    // Metrics are in core_ (RoleHostCore) — single source of truth.
    // See core_.out_written_, core_.in_received_, core_.drops_, core_.iteration_count_, core_.last_cycle_work_us_.
};

} // namespace pylabhub::processor
