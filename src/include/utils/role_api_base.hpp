#pragma once
/**
 * @file role_api_base.hpp
 * @brief RoleAPIBase — unified, language-neutral role API.
 *
 * Pure C++ interface for all role operations: identity, control, broker queries,
 * messaging, inbox, diagnostics, spinlocks, custom metrics, flexzone, checksum.
 *
 * Direction-agnostic: holds optional Producer* and Consumer*. Methods that operate
 * on a missing side return safe defaults. The role is defined by which pointers
 * the role host wires at construction, not by class hierarchy.
 *
 * ABI-stable: Pimpl, no virtual methods, no inline bodies. Part of pylabhub-utils.
 *
 * @see role_api.hpp for the type-safe C++ template layer on top of this base.
 * @see docs/tech_draft/role_api_base_design.md for the full design document.
 */

#include "pylabhub_utils_export.h"
#include "utils/data_block_policy.hpp"     // hub::ChecksumPolicy
#include "utils/json_fwd.hpp"
#include "utils/loop_timing_policy.hpp"    // LoopTimingPolicy, compute_short_timeout, compute_next_deadline
#include "utils/role_host_core.hpp"        // RoleHostCore, StateValue
#include "utils/schema_types.hpp"          // hub::SchemaSpec (for InboxOpenResult)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pylabhub::hub
{
class Producer;
class Consumer;
class Messenger;
class InboxQueue;
class InboxClient;
class SharedSpinLock;
class BrokerRequestChannel;
} // namespace pylabhub::hub

namespace pylabhub::scripting
{

class ScriptEngine;   // forward declaration (defined in script_engine.hpp)

/// Identifies which side of the data path (Tx = producer/output, Rx = consumer/input).
/// Used by spinlock and potentially other side-specific accessors.
enum class ChannelSide : uint8_t { Tx = 0, Rx = 1 };

// ============================================================================
// AcquireContext — timing state for one cycle, computed by the shared frame
// ============================================================================

/// Passed to RoleCycleOps::acquire() so role code never computes timeouts.
struct AcquireContext
{
    std::chrono::milliseconds              short_timeout;
    std::chrono::microseconds              short_timeout_us;
    std::chrono::steady_clock::time_point  deadline;
    bool                                   is_max_rate;
};

// ============================================================================
// retry_acquire — shared inner retry utility
// ============================================================================

/// Inner retry loop used by RoleCycleOps::acquire() for the primary acquire.
///
/// Calls try_once(short_timeout) repeatedly until:
///   - try_once returns non-null (success)
///   - is_max_rate (single attempt only)
///   - core signals shutdown or process exit
///   - remaining time until deadline < short_timeout_us
///
/// First cycle (deadline == time_point::max()): retries indefinitely
/// until success or shutdown (per loop_design_unified.md §3 Step A).
PYLABHUB_UTILS_EXPORT void *retry_acquire(
    const AcquireContext &ctx,
    RoleHostCore &core,
    const std::function<void *(std::chrono::milliseconds)> &try_once);

// ============================================================================
// RoleCycleOps — role-specific operations plugged into the shared loop frame
// ============================================================================

/// Each role (producer, consumer, processor) provides a concrete subclass
/// that implements the acquire/invoke/commit slot. The shared loop frame
/// handles: outer loop condition, inner retry, deadline wait, drain,
/// metrics, next deadline.
struct PYLABHUB_UTILS_EXPORT RoleCycleOps
{
    virtual ~RoleCycleOps() = default;

    /// Step A: Acquire data for this cycle.
    /// Use retry_acquire() for the primary acquire.
    /// Returns true if the cycle has data that gates the deadline wait.
    ///   Producer/Consumer: true when slot acquired.
    ///   Processor: always true (maintains cadence on idle cycles).
    virtual bool acquire(const AcquireContext &ctx) = 0;

    /// Step B': Release/discard acquired resources on shutdown break.
    virtual void cleanup_on_shutdown() = 0;

    /// Step D+E: Invoke engine callback with drained messages, then
    /// commit/discard/release. Returns false to request loop stop
    /// (e.g., stop_on_script_error).
    virtual bool invoke_and_commit(std::vector<IncomingMessage> &msgs) = 0;

    /// Post-loop cleanup (e.g., release processor's held_input).
    virtual void cleanup_on_exit() = 0;
};

// ============================================================================
// LoopConfig — timing parameters for the shared data loop
// ============================================================================

struct LoopConfig
{
    double            period_us{0};
    LoopTimingPolicy  loop_timing{LoopTimingPolicy::MaxRate};
    double            queue_io_wait_timeout_ratio{0.1};
};

// ============================================================================
// RoleAPIBase
// ============================================================================

class PYLABHUB_UTILS_EXPORT RoleAPIBase
{
  public:
    explicit RoleAPIBase(RoleHostCore &core);
    ~RoleAPIBase();

    RoleAPIBase(const RoleAPIBase &) = delete;
    RoleAPIBase &operator=(const RoleAPIBase &) = delete;
    RoleAPIBase(RoleAPIBase &&) noexcept;
    RoleAPIBase &operator=(RoleAPIBase &&) noexcept;

    // ── Host wiring (called once by role host after setup_infrastructure_) ────

    void set_producer(hub::Producer *p);
    void set_consumer(hub::Consumer *c);
    void set_messenger(hub::Messenger *m);
    void set_inbox_queue(hub::InboxQueue *q);
    void set_role_tag(std::string tag);
    void set_uid(std::string uid);
    void set_name(std::string name);
    void set_channel(std::string c);
    void set_out_channel(std::string c);
    void set_log_level(std::string l);
    void set_script_dir(std::string d);
    void set_role_dir(std::string d);
    void set_engine(ScriptEngine *e);
    void set_checksum_policy(hub::ChecksumPolicy p);
    void set_stop_on_script_error(bool v);

    // ── Event callback wiring ────────────────────────────────────────────
    //
    // Wires all Consumer/Producer/Messenger event callbacks to
    // core_.enqueue_message() or core_.request_stop(). Inspects which
    // pointers are non-null and wires the appropriate callbacks.
    // Call after set_producer/set_consumer/set_messenger/set_channel.
    //
    // Replaces the per-role-host copy-paste of on_channel_closing,
    // on_force_shutdown, on_zmq_data, on_producer_message, on_channel_error,
    // on_consumer_joined/left/message, on_peer_dead, on_hub_dead, etc.
    void wire_event_callbacks();

    // ── Identity ──────────────────────────────────────────────────────────────

    [[nodiscard]] const std::string &role_tag() const;
    [[nodiscard]] const std::string &uid() const;
    [[nodiscard]] const std::string &name() const;
    [[nodiscard]] const std::string &channel() const;
    [[nodiscard]] const std::string &out_channel() const;
    [[nodiscard]] const std::string &log_level() const;
    [[nodiscard]] const std::string &script_dir() const;
    [[nodiscard]] const std::string &role_dir() const;
    [[nodiscard]] std::string logs_dir() const;
    [[nodiscard]] std::string run_dir() const;
    [[nodiscard]] hub::ChecksumPolicy checksum_policy() const;
    [[nodiscard]] bool stop_on_script_error() const;

    // ── Control ───────────────────────────────────────────────────────────────

    void log(const std::string &level, const std::string &msg);
    void stop();
    void set_critical_error();
    [[nodiscard]] bool critical_error() const;
    [[nodiscard]] std::string stop_reason() const;

    // ── Band pub/sub messaging (HEP-CORE-0030) ────────────────────────────────

    /// Join a named band. Auto-creates if it doesn't exist.
    [[nodiscard]] std::optional<nlohmann::json>
    band_join(const std::string &channel);

    /// Leave a band.
    bool band_leave(const std::string &channel);

    /// Send JSON message to all band members.
    void band_broadcast(const std::string &channel,
                        const nlohmann::json &body);

    /// Query band member list.
    [[nodiscard]] std::optional<nlohmann::json>
    band_members(const std::string &channel);

    // ── Inbox client management ───────────────────────────────────────────────

    struct InboxOpenResult
    {
        std::shared_ptr<hub::InboxClient> client;
        hub::SchemaSpec spec;
        std::string packing;
        size_t item_size{0};
    };

    [[nodiscard]] std::optional<InboxOpenResult>
    open_inbox_client(const std::string &target_uid);
    [[nodiscard]] bool wait_for_role(const std::string &uid, int timeout_ms = 5000);
    void close_all_inbox_clients();

    // ── Output side (safe defaults when no Producer) ──────────────────────────

    [[nodiscard]] void *write_flexzone();
    [[nodiscard]] const void *read_flexzone() const;
    [[nodiscard]] size_t flexzone_size() const;
    bool update_flexzone_checksum();
    [[nodiscard]] uint64_t out_slots_written() const;
    [[nodiscard]] uint64_t out_drop_count() const;
    [[nodiscard]] size_t out_capacity() const;
    [[nodiscard]] std::string out_policy() const;

    // ── Input side (safe defaults when no Consumer) ───────────────────────────

    [[nodiscard]] uint64_t in_slots_received() const;
    [[nodiscard]] uint64_t last_seq() const;
    [[nodiscard]] size_t in_capacity() const;
    [[nodiscard]] std::string in_policy() const;
    void set_verify_checksum(bool enable);

    // ── Diagnostics ───────────────────────────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const;
    [[nodiscard]] uint64_t loop_overrun_count() const;
    [[nodiscard]] uint64_t last_cycle_work_us() const;
    [[nodiscard]] uint64_t ctrl_queue_dropped() const;

    // ── Schema sizes (logical = C struct, no page alignment) ──────────────────

    [[nodiscard]] size_t slot_logical_size(std::optional<ChannelSide> side = std::nullopt) const;
    [[nodiscard]] size_t flexzone_logical_size(std::optional<ChannelSide> side = std::nullopt) const;

    // ── Spinlocks (delegates to whichever side has SHM) ───────────────────────

    [[nodiscard]] hub::SharedSpinLock get_spinlock(size_t index,
                                                   std::optional<ChannelSide> side = std::nullopt);
    [[nodiscard]] uint32_t spinlock_count(std::optional<ChannelSide> side = std::nullopt) const;

    // ── Custom metrics ────────────────────────────────────────────────────────

    void report_metric(const std::string &key, double value);
    void report_metrics(const std::unordered_map<std::string, double> &kv);
    void clear_custom_metrics();

    // ── Metrics snapshot (data-driven, no virtual) ────────────────────────────

    [[nodiscard]] nlohmann::json snapshot_metrics_json() const;

    // ── Shared script state (delegates to RoleHostCore) ─────────────────────

    using StateValue = RoleHostCore::StateValue;
    void set_shared_data(const std::string &key, StateValue value);
    [[nodiscard]] std::optional<StateValue> get_shared_data(const std::string &key) const;
    void remove_shared_data(const std::string &key);
    void clear_shared_data();

    // ── Thread manager ────────────────────────────────────────────────────
    //
    // Lightweight thread registry. Every spawned thread gets ThreadEngineGuard
    // and shutdown check via core_.is_running(). The ctrl thread is the first
    // managed thread. Future worker threads use the same interface.

    /// Spawn a named managed thread. The body runs inside a ThreadEngineGuard.
    void spawn_thread(const std::string &name, std::function<void()> body);

    /// Join all managed threads in reverse spawn order.
    void join_all_threads();

    /// Number of managed threads (for diagnostics).
    [[nodiscard]] size_t thread_count() const;

    // ── Broker thread ─────────────────────────────────────────────────────

    /// Set the BrokerRequestChannel (owned externally by role host).
    void set_broker_channel(hub::BrokerRequestChannel *bc);

    struct BrokerThreadConfig
    {
        int  heartbeat_interval_ms{5000};
        bool report_metrics{false};
    };

    /// Spawn the broker thread: runs BrokerRequestChannel poll loop with
    /// iteration-gated heartbeat + optional metrics report + on_heartbeat.
    void start_broker_thread(const BrokerThreadConfig &cfg);

    // ── P2C comm thread (polls Producer/Consumer sockets) ────────────────

    /// Spawn a thread that polls P2C sockets (peer join/leave/dead, ctrl msgs).
    /// Temporary: will be replaced by RoleCommunicationChannel.
    void start_comm_thread();

    // ── Inbox drain (Step C of the data loop) ──────────────────────────────
    //
    // Drains all pending inbox messages and invokes engine->invoke_on_inbox()
    // for each. Called by run_data_loop() right before the role's invoke.
    // Requires set_engine() to have been called.
    void drain_inbox_sync();

    // ── Unified data loop ─────────────────────────────────────────────────
    //
    // Runs the shared loop frame: outer loop condition, inner retry,
    // deadline wait, drain, metrics, next deadline. Role-specific
    // acquire/invoke/commit is delegated to the RoleCycleOps.
    // Blocks until shutdown.
    void run_data_loop(const LoopConfig &cfg, RoleCycleOps &ops);

    // ── Infrastructure access (for engine binding layers) ─────────────────────

    [[nodiscard]] RoleHostCore *core() const;
    [[nodiscard]] hub::Producer *producer() const;
    [[nodiscard]] hub::Consumer *consumer() const;
    [[nodiscard]] hub::InboxQueue *inbox_queue() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::scripting
