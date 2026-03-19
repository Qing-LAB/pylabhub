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

#include "consumer_config.hpp"
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
class QueueReader;
class InboxQueue;
} // namespace pylabhub::hub

namespace pylabhub::consumer
{

class ConsumerRoleHost
{
  public:
    ConsumerRoleHost() = default;
    ~ConsumerRoleHost();

    // Non-copyable, non-movable (owns threads).
    ConsumerRoleHost(const ConsumerRoleHost &) = delete;
    ConsumerRoleHost &operator=(const ConsumerRoleHost &) = delete;
    ConsumerRoleHost(ConsumerRoleHost &&) = delete;
    ConsumerRoleHost &operator=(ConsumerRoleHost &&) = delete;

    // ── Configuration (call before startup_) ────────────────────────────────

    void set_engine(std::unique_ptr<scripting::ScriptEngine> engine);
    void set_config(ConsumerConfig config);
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
    [[nodiscard]] const ConsumerConfig &config() const { return config_; }

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
    ConsumerConfig                         config_;
    std::unique_ptr<scripting::ScriptEngine> engine_;
    bool                                   validate_only_{false};
    std::atomic<bool>                      script_load_ok_{false};

    // Worker thread.
    std::thread                            worker_thread_;
    std::promise<bool>                     ready_promise_;

    // Infrastructure (created on worker thread in setup_infrastructure_).
    hub::Messenger                         in_messenger_;
    std::optional<hub::Consumer>           in_consumer_;

    /// SHM transport: owned QueueReader wrapping DataBlockConsumer.
    /// ZMQ transport: nullptr (queue_reader_ points to Consumer-owned ZmqQueue).
    std::unique_ptr<hub::QueueReader>      shm_queue_;
    /// Non-owning pointer to the active QueueReader (shm_queue_.get() or Consumer::queue_reader()).
    hub::QueueReader                      *queue_reader_{nullptr};

    std::unique_ptr<hub::InboxQueue>       inbox_queue_;
    std::thread                            ctrl_thread_;

    // Schema info (resolved from config during setup).
    // fz_spec is stored in core_.fz_spec (shared with engine for flexzone exposure).
    scripting::SchemaSpec                  slot_spec_;
    size_t                                 schema_slot_size_{0};
    std::string                            inbox_type_name_;

    // Metrics (atomic, written by worker, read by ctrl_thread_ heartbeat).
    std::atomic<uint64_t>                  in_received_{0};
    std::atomic<uint64_t>                  last_seq_{0};
    std::atomic<uint64_t>                  iteration_count_{0};
    std::atomic<uint64_t>                  last_cycle_work_us_{0};
};

} // namespace pylabhub::consumer
