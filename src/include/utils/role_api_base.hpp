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
#include "utils/timeout_constants.hpp"
#include "utils/broker_request_comm.hpp"   // hub::BrokerRequestComm (for Config in start_ctrl_thread)
#include "utils/config/inbox_config.hpp"   // config::InboxConfig (for CtrlThreadConfig)
#include "utils/data_block_policy.hpp"     // hub::ChecksumPolicy
#include "utils/hub_producer.hpp"          // hub::ProducerOptions (for build_tx_queue)
#include "utils/hub_consumer.hpp"          // hub::ConsumerOptions (for build_rx_queue)
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
class InboxQueue;
class InboxClient;
class SharedSpinLock;
// BrokerRequestComm: full definition from broker_request_comm.hpp (needed for Config in start_ctrl_thread).
} // namespace pylabhub::hub

namespace pylabhub::utils
{
class ThreadManager;   // fwd decl; full in utils/thread_manager.hpp
} // namespace pylabhub::utils

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
    /// @param core      RoleHostCore owned by the role host (lifetime > api).
    /// @param role_tag  Role type tag: "prod" / "cons" / "proc". Non-empty.
    /// @param uid       Role instance uid (e.g. "PROD-SENSOR-00000001"). Non-empty.
    ///
    /// Identity (role_tag + uid) is required at construction time so the
    /// role's ThreadManager is built immediately as a member — no
    /// two-stage init, no runtime "did you forget to initialize" check.
    /// The compile-time signature enforces that a caller cannot construct
    /// a RoleAPIBase without providing both halves of role identity.
    /// Throws std::invalid_argument if either string is empty.
    RoleAPIBase(RoleHostCore &core,
                std::string   role_tag,
                std::string   uid);
    ~RoleAPIBase();

    RoleAPIBase(const RoleAPIBase &) = delete;
    RoleAPIBase &operator=(const RoleAPIBase &) = delete;
    RoleAPIBase(RoleAPIBase &&) noexcept;
    RoleAPIBase &operator=(RoleAPIBase &&) noexcept;

    // ── Host wiring (called once by role host after setup_infrastructure_) ────
    //
    // Note: set_role_tag + set_uid are GONE. Both are now ctor args and
    // immutable after construction. All remaining setters are for mutable
    // wiring state (infrastructure pointers, script paths, policies).

    /// Build the output-side queue (Tx). Creates and owns a hub::Producer
    /// internally; the unified QueueWriter handle is cached on Impl.
    /// @return true on success. On failure, no queue is wired.
    [[nodiscard]] bool build_tx_queue(const hub::ProducerOptions &opts);

    /// Build the input-side queue (Rx). Creates and owns a hub::Consumer
    /// internally; the unified QueueReader handle is cached on Impl.
    [[nodiscard]] bool build_rx_queue(const hub::ConsumerOptions &opts);

    /// Start the Tx/Rx queues. Returns false if the side is not wired or
    /// the queue start() failed.
    [[nodiscard]] bool start_tx_queue();
    [[nodiscard]] bool start_rx_queue();

    /// Reset metrics counters on the Tx/Rx queues. No-op if not wired.
    void reset_tx_queue_metrics();
    void reset_rx_queue_metrics();

    /// Sync flexzone checksum on the Tx side (SHM only). No-op otherwise.
    void sync_tx_flexzone_checksum();

    /// True iff the side is wired AND backed by SHM (not ZMQ).
    [[nodiscard]] bool tx_has_shm() const noexcept;
    [[nodiscard]] bool rx_has_shm() const noexcept;

    /// Close/teardown both sides. Idempotent.
    void close_queues();

    void set_inbox_queue(hub::InboxQueue *q);
    void set_name(std::string name);
    void set_channel(std::string c);
    void set_out_channel(std::string c);
    void set_log_level(std::string l);
    void set_script_dir(std::string d);
    void set_role_dir(std::string d);
    void set_engine(ScriptEngine *e);
    void set_checksum_policy(hub::ChecksumPolicy p);
    void set_stop_on_script_error(bool v);

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

    // ── Output side (safe defaults when no output wired) ──────────────────────
    //
    // Flat data-plane verbs. Return nullptr / no-op when the role has no
    // output side. Callers who have configured an output side will never
    // see nullptr except when the underlying queue genuinely has no slot
    // available within the timeout. See loop_design_unified.md for timing.

    [[nodiscard]] void *write_acquire(std::chrono::milliseconds timeout) noexcept;
    void               write_commit() noexcept;
    void               write_discard() noexcept;
    /// Flexzone pointer for the given side (Tx or Rx). Single region per
    /// channel, fully read+write on both endpoints per HEP-CORE-0002 §2.2.
    /// Returns nullptr when the side is not wired or the channel has no
    /// flexzone configured.
    [[nodiscard]] void  *flexzone(ChannelSide side);
    /// Physical flexzone size in bytes for the given side. 0 when not wired.
    [[nodiscard]] size_t flexzone_size(ChannelSide side) const noexcept;
    bool update_flexzone_checksum();
    bool sync_flexzone_checksum();
    [[nodiscard]] size_t write_item_size() const noexcept;
    [[nodiscard]] uint64_t out_slots_written() const;
    [[nodiscard]] uint64_t out_drop_count() const;
    [[nodiscard]] size_t out_capacity() const;
    [[nodiscard]] std::string out_policy() const;

    // ── Input side (safe defaults when no input wired) ────────────────────────
    //
    // Flat data-plane verbs. Return nullptr / no-op when the role has no
    // input side.

    [[nodiscard]] const void *read_acquire(std::chrono::milliseconds timeout) noexcept;
    void                      read_release() noexcept;
    [[nodiscard]] size_t       read_item_size() const noexcept;
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

    /// Access the role's thread manager. Always valid — constructed in
    /// the RoleAPIBase ctor alongside role_tag/uid. All role-scope
    /// threads (worker / ctrl / inbox drainer / future) live under this
    /// one manager — same dynamic lifecycle module
    /// "ThreadManager:{role_tag}:{uid}", same bounded-join, same
    /// process-wide leak aggregator. Usage:
    ///
    ///   api_->thread_manager().spawn("worker",
    ///       [&] { scripting::ThreadEngineGuard g(*engine_); worker_main_(); });
    ///
    /// No throw path — invariant "manager is always ready" enforced by
    /// the ctor signature (role_tag + uid are required positional args).
    [[nodiscard]] pylabhub::utils::ThreadManager &thread_manager();

    // ── Broker communication (control plane) ────────────────────────────────

    /// Set the BrokerRequestComm (owned externally by role host).
    void set_broker_comm(hub::BrokerRequestComm *bc);

    /// Configuration for the control thread (broker communication).
    struct CtrlThreadConfig
    {
        int  heartbeat_interval_ms{::pylabhub::kDefaultHeartbeatIntervalMs};
        bool report_metrics{false};

        /// Registration payload (REG_REQ for producers, CONSUMER_REG_REQ for
        /// consumers). Empty = skip registration. Role host builds this JSON
        /// from its config before calling start_ctrl_thread().
        nlohmann::json producer_reg_opts{};   ///< Non-empty → send REG_REQ
        nlohmann::json consumer_reg_opts{};   ///< Non-empty → send CONSUMER_REG_REQ

        /// Inbox config (optional). If has_inbox(), start_ctrl_thread
        /// appends inbox fields to the registration payload automatically.
        /// actual_endpoint is queried from the InboxQueue set via set_inbox_queue().
        std::optional<config::InboxConfig> inbox{};
    };

    /// Connect to broker, start the control thread (poll loop + heartbeat),
    /// and register with the broker.
    ///
    /// Sequence:
    ///   1. broker_comm_->connect(connect_cfg)
    ///   2. Wire on_notification + on_hub_dead callbacks
    ///   3. Spawn "ctrl" thread (periodic heartbeat + poll loop)
    ///   4. Send REG_REQ / CONSUMER_REG_REQ (blocking, from main thread)
    ///
    /// @param connect_cfg  BRC connection config (endpoint, pubkey, etc.)
    /// @param cfg          Heartbeat interval, metrics, registration payloads.
    /// @return true if connection and registration succeeded.
    bool start_ctrl_thread(const hub::BrokerRequestComm::Config &connect_cfg,
                           const CtrlThreadConfig &cfg);

    /// Explicitly deregister from broker. Call BEFORE join_all_threads()
    /// while the ctrl thread is still running to process the command.
    /// Sends DEREG_REQ and/or CONSUMER_DEREG_REQ for whatever was registered
    /// in start_ctrl_thread().
    void deregister_from_broker();

    // ── Broker protocol helpers (require ctrl thread running) ────────────

    /// Register a producer channel (REG_REQ → REG_ACK).
    [[nodiscard]] std::optional<nlohmann::json>
    register_producer_channel(const nlohmann::json &opts, int timeout_ms = 5000);

    /// Discover a channel (DISC_REQ → DISC_ACK).
    [[nodiscard]] std::optional<nlohmann::json>
    discover_channel(const std::string &channel, int timeout_ms = 10000);

    /// Register as consumer (CONSUMER_REG_REQ → CONSUMER_REG_ACK).
    [[nodiscard]] std::optional<nlohmann::json>
    register_consumer(const nlohmann::json &opts, int timeout_ms = 5000);

    /// Deregister a producer channel (DEREG_REQ).
    bool deregister_producer_channel(const std::string &channel, int timeout_ms = 5000);

    /// Deregister a consumer (CONSUMER_DEREG_REQ).
    bool deregister_consumer(const std::string &channel, int timeout_ms = 5000);

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
    [[nodiscard]] hub::InboxQueue *inbox_queue() const;

    /// Side-presence checks for engines/callers that need to gate script-facing
    /// method registration on which side is wired. Supersedes the producer()/
    /// consumer() pointer accessors above.
    [[nodiscard]] bool has_tx_side() const noexcept;   ///< True iff Tx queue wired.
    [[nodiscard]] bool has_rx_side() const noexcept;   ///< True iff Rx queue wired.

    /// Queue metrics for the given side. Empty QueueMetrics when the side
    /// is not wired. Routes to QueueReader::metrics() / QueueWriter::metrics().
    [[nodiscard]] hub::QueueMetrics queue_metrics(ChannelSide side) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;

    // ── Ctrl thread private helpers (called from within the ctrl thread) ──
    // Access pImpl members directly — no bare pointers cross thread boundary.

    void on_heartbeat_tick_();
    void on_metrics_report_tick_();
};

} // namespace pylabhub::scripting
