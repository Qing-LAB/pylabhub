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

#include "producer_config.hpp"
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
class Producer;
class QueueWriter;
class InboxQueue;
} // namespace pylabhub::hub

namespace pylabhub::producer
{

class ProducerRoleHost
{
  public:
    ProducerRoleHost() = default;
    ~ProducerRoleHost();

    // Non-copyable, non-movable (owns threads).
    ProducerRoleHost(const ProducerRoleHost &) = delete;
    ProducerRoleHost &operator=(const ProducerRoleHost &) = delete;
    ProducerRoleHost(ProducerRoleHost &&) = delete;
    ProducerRoleHost &operator=(ProducerRoleHost &&) = delete;

    // ── Configuration (call before startup_) ────────────────────────────────

    void set_engine(std::unique_ptr<scripting::ScriptEngine> engine);
    void set_config(ProducerConfig config);
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
    [[nodiscard]] const ProducerConfig &config() const { return config_; }

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
    ProducerConfig                         config_;
    std::unique_ptr<scripting::ScriptEngine> engine_;
    bool                                   validate_only_{false};
    std::atomic<bool>                      script_load_ok_{false};

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
    // fz_spec is stored in core_.fz_spec (shared with engine for flexzone exposure).
    scripting::SchemaSpec                  slot_spec_;
    size_t                                 schema_slot_size_{0};
    std::string                            inbox_type_name_;

    // Metrics (atomic, written by worker, read by ctrl_thread_ heartbeat).
    std::atomic<uint64_t>                  out_written_{0};
    std::atomic<uint64_t>                  drops_{0};
    std::atomic<uint64_t>                  iteration_count_{0};
    std::atomic<uint64_t>                  last_cycle_work_us_{0};
};

} // namespace pylabhub::producer
