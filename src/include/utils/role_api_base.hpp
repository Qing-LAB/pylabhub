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
#include "utils/broker_request_comm.hpp" // hub::BrokerRequestComm (handler/Class A/B/D dispatch returns this ptr)
#include "utils/config/inbox_config.hpp" // config::InboxConfig (append_inbox_to_reg)
#include "utils/data_block.hpp"          // DataBlockConfig (for TxQueueOptions::shm_config)
#include "utils/data_block_policy.hpp"   // hub::ChecksumPolicy
#include "utils/hub_state.hpp"           // hub::ChannelTopology
#include "utils/hub_zmq_queue.hpp"       // hub::OverflowPolicy, kZmqDefaultBufferDepth
#include "utils/json_fwd.hpp"
#include "utils/role_host_core.hpp" // RoleHostCore, StateValue
#include "utils/schema_types.hpp" // hub::SchemaSpec (for InboxOpenResult, TxQueueOptions, RxQueueOptions)

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
class InboxQueue;
class InboxClient;
class SharedSpinLock;
// BrokerRequestComm: full definition from broker_request_comm.hpp (handler/Class A/B/D dispatch
// returns this ptr).

// ============================================================================
// Queue options
// ============================================================================
//
// These structs are the parameter-object for RoleAPIBase::build_tx_queue()
// and RoleAPIBase::build_rx_queue().  They live with the API that consumes
// them — not in a separate hub_{producer,consumer}.hpp header — because
// they have no other consumer.  Previously:
//   - hub::Producer class was eliminated (post L3.γ A6.3); only the config
//     struct survived in hub_producer.hpp
//   - hub::Consumer class was eliminated; only the config struct survived
//     in hub_consumer.hpp
// Consolidating them here retires those two legacy headers and names the
// structs by direction (Tx/Rx) to match the API method names.
//
// Identity fields (channel, uid, name) come from RoleAPIBase state, not
// opts — so building a queue requires the api's identity already set
// (set_channel / set_out_channel / set_name before build_{tx,rx}_queue).
//
// Schema + packing: single source of truth via SchemaSpec (spec.fields
// and spec.packing).  Schema hash is auto-computed from specs at build
// time — no redundant schema_hash field.

/// Configuration for RoleAPIBase::build_tx_queue().  Output side
/// (producer / processor-out).
struct TxQueueOptions
{
    /// Channel topology per HEP-CORE-0017 §3.3.0.  Combined with
    /// `data_transport` this picks the row of the §3.3.0 decision
    /// matrix that the queue will use.  Default `OneToOne`
    /// matches the pre-topology-model behavior (producer binds).
    /// Under `FanIn` the producer is the dialing side and does
    /// NOT bind; under `FanOut` and `OneToOne` the producer is the
    /// binding side.  The queue factory reads this field to pick
    /// socket type + bind/connect direction; `zmq_bind` below is
    /// redundant with it and retires alongside role code migration.
    ChannelTopology topology{ChannelTopology::OneToOne};

    bool has_shm{false};
    DataBlockConfig shm_config{};

    /// Slot + flexzone schemas — single source for fields + packing.
    SchemaSpec slot_spec{};
    SchemaSpec fz_spec{};

    // Transport (HEP-CORE-0021)
    std::string data_transport{"shm"};
    std::string zmq_node_endpoint{};
    bool zmq_bind{true};
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};
    OverflowPolicy zmq_overflow_policy{OverflowPolicy::Drop};

    // Queue policy
    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    bool flexzone_checksum{true};
    bool always_clear_slot{true};

    /// ThreadManager owner id for internal threads.  Empty → auto-derived
    /// as "<short_tag>:<uid>:tx" from RoleAPIBase identity.  Set
    /// explicitly only for direct-factory tests that bypass RoleAPIBase.
    std::string instance_id{};

    /// HEP-CORE-0041 1i-mig-2: borrowed SHM fd from the role host's
    /// `IShmCapabilityProducer` (substep 1b backend).  On a SHM channel
    /// (`has_shm` + `data_transport=="shm"`), `build_tx_queue` REQUIRES
    /// this field to be `>= 0` — it selects
    /// `ShmQueue::create_writer_standby` + `set_shm_capability_fd(this fd)`
    /// + `start()`, wrapping the pre-allocated memfd via the substep 1f
    /// fd-source factory.  An unset value (-1) on a SHM channel is a
    /// build error.  The legacy `shared_secret`-based path retired in
    /// #275-S3.
    ///
    /// **Borrowed reference.**  The role host (L1 transport owner)
    /// retains ownership of the underlying fd; ShmQueue is non-owning
    /// and the DataBlock fd-source ctor dups internally.  Caller MUST
    /// keep the L1 transport alive at least as long as the ShmQueue.
    ///
    /// When `has_shm == false`, this field is ignored.
    int shm_capability_fd{-1};
};

// `ProducerPeer` lives in `hub_zmq_queue.hpp` (next to the
// `add_producer_peer` / `remove_producer_peer` API that consumes it)
// to avoid an include cycle.  Imported here via the existing
// `#include "utils/hub_zmq_queue.hpp"` at the top of this file.

/// Configuration for RoleAPIBase::build_rx_queue().  Input side
/// (consumer / processor-in).
struct RxQueueOptions
{
    /// Channel topology per HEP-CORE-0017 §3.3.0.  Under `FanIn`
    /// the consumer is the binding side (owns the endpoint);
    /// under `FanOut` and `OneToOne` the consumer is the dialing
    /// side.  Default `OneToOne` matches the pre-topology-model
    /// behavior (consumer dials).  The queue factory reads this
    /// field to pick socket type + bind/connect direction.
    ChannelTopology topology{ChannelTopology::OneToOne};

    /// HEP-CORE-0041 1i-mig-4 (#272) — SHM capability transport fd
    /// (anonymous memfd received via `SCM_RIGHTS` from the producer).
    /// Symmetric with `TxQueueOptions::shm_capability_fd`.
    ///
    /// **Production flow:** the field stays `-1` at `build_rx_queue`
    /// time (the consumer doesn't have the fd until AFTER REG_ACK
    /// arrives carrying the producer's endpoint).  `build_rx_queue`'s
    /// SHM branch builds the ShmQueue in Standby.  Then
    /// `apply_consumer_reg_ack` dials the producer's endpoint, runs
    /// the §5.5 handshake, recvs the fd via SCM_RIGHTS, stores the
    /// `IShmCapabilityConsumer` on `RoleAPIBase::Impl` (D1 ownership
    /// decision), and calls `rx_queue->set_shm_capability_fd(fd)` +
    /// `start()` directly — bypassing this field on the production
    /// path.
    ///
    /// **Test path:** L2/L3 tests MAY pre-populate this field with
    /// a synthetic memfd to build an Active SHM rx queue at
    /// construction time (analogous to how `producer_peers` may be
    /// pre-populated for ZMQ tests).  When set, `build_rx_queue`
    /// drives Standby → Active immediately via set_shm_capability_fd
    /// + start.
    int shm_capability_fd{-1};

    /// Slot + flexzone schemas — single source for fields + packing.
    SchemaSpec slot_spec{};
    SchemaSpec fz_spec{};

    // Transport (HEP-CORE-0021)
    std::string data_transport{"shm"};
    std::string shm_name{};
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};

    /// Consumer's ZMQ bind endpoint on the binding side (fan-in
    /// consumer per HEP-CORE-0017 §3.3.0).  On dialing-side topologies
    /// (fan-out consumer, one-to-one consumer) this field is unused —
    /// the peer endpoint arrives on CONSUMER_REG_ACK.data_endpoint.
    /// May be `tcp://host:0` for ephemeral bind; the queue resolves
    /// the actual port and publishes it via ENDPOINT_UPDATE_REQ after
    /// bind.
    std::string zmq_node_endpoint{};

    /// HEP-CORE-0017 §3.3 + HEP-CORE-0036 §4.1 + §6.4 dynamic-membership
    /// producer set populated from `CONSUMER_REG_ACK.producers[]`.  At
    /// `build_rx_queue` time, an empty `producer_peers` is the canonical
    /// state — `pull_from` accepts empty endpoint + empty serverkey and
    /// the queue enters Standby (HEP-CORE-0036 §6.7).
    /// `apply_consumer_reg_ack(ack)` on the RoleAPIBase then drives
    /// Standby → Configured → Active via the polymorphic
    /// `QueueReader::apply_master_approval`.  Test/legacy paths MAY
    /// pre-populate `producer_peers` here to enter Configured at
    /// construction; production never does — the broker is the master.
    std::vector<ProducerPeer> producer_peers;

    // Queue policy
    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    bool flexzone_checksum{true};

    /// Empty → auto-derived as "<short_tag>:<uid>:rx" from RoleAPIBase.
    std::string instance_id{};
};

} // namespace pylabhub::hub

namespace pylabhub::utils
{
class ThreadManager; // fwd decl; full in utils/thread_manager.hpp
} // namespace pylabhub::utils

namespace pylabhub::scripting
{

class ScriptEngine; // forward declaration (defined in script_engine.hpp)

/// HEP-CORE-0036 §I11 + §6.5 — one entry in the script-side
/// authorized-peer view.  Producer-side: an authorized consumer of
/// the channel.  Consumer-side: an authorized producer of the
/// channel.  Mirrors the wire shape of `GET_CHANNEL_AUTH_ACK.allowlist`
/// (§6.5) and `CONSUMER_REG_ACK.producers[]` (§6.4): a `{role_uid,
/// pubkey}` pair, where `pubkey` is the Z85-encoded CURVE identity
/// and `role_uid` is the operator-assigned role identifier.
struct PYLABHUB_UTILS_EXPORT AllowedPeer
{
    std::string role_uid;
    std::string pubkey;
};
class RoleHandler; // fwd — defined in utils/role_handler.hpp.  Wave-B
                   // M4c: RoleAPIBase owns the handler via
                   // unique_ptr; the handler holds the role's
                   // presence list + deduplicated HubConnection
                   // vector + routing indexes.

/// Identifies which side of the data path (Tx = producer/output, Rx = consumer/input).
/// Used by spinlock and potentially other side-specific accessors.
enum class ChannelSide : uint8_t
{
    Tx = 0,
    Rx = 1
};

// ============================================================================
// RoleAPIBase
// ============================================================================

#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
namespace test
{
class RoleAPIBaseTestAccess;
}
#endif

class PYLABHUB_UTILS_EXPORT RoleAPIBase
{
#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
    // L2 test access — see `tests/test_framework/role_api_base_test_access.h`.
    // The test helper installs a legitimately-constructed `RoleHandler`
    // (with Presences whose `registration_state` the test sets directly
    // via the public atomic) into `pImpl->handler_`.  Production
    // protocol invariants — including HEP-CORE-0036 §8.2's
    // `any_presence_authorized()` gate — run unchanged: the gate
    // scans the installed presences and reports `true` only when the
    // test has put a Presence in `Authorized` state.  No bypass.
    //
    // Double-gated: both `PYLABHUB_BUILD_TESTS` (CMake `-DBUILD_TESTS=ON`)
    // AND `!defined(NDEBUG)` (Debug build).  A misconfigured Release
    // build with -DBUILD_TESTS=ON still gets the friend + private
    // setter PHYSICALLY ABSENT from the compiled binary — the
    // protocol-gate surface does not widen.  Mirrors the
    // `test_set_*` mutator pattern at role_host_core.hpp:581.
    friend class test::RoleAPIBaseTestAccess;
#endif

  public:
    /// @param core      RoleHostCore owned by the role host (lifetime > api).
    /// @param short_tag  Role type tag: "prod" / "cons" / "proc". Non-empty.
    /// @param uid       Role instance uid (e.g. "prod.sensor.uid00000001"). Non-empty.
    ///
    /// Identity (short_tag + uid) is required at construction time so the
    /// role's ThreadManager is built immediately as a member — no
    /// two-stage init, no runtime "did you forget to initialize" check.
    /// The compile-time signature enforces that a caller cannot construct
    /// a RoleAPIBase without providing both halves of role identity.
    /// Throws std::invalid_argument if either string is empty.
    RoleAPIBase(RoleHostCore &core, std::string short_tag, std::string uid);
    ~RoleAPIBase();

    RoleAPIBase(const RoleAPIBase &) = delete;
    RoleAPIBase &operator=(const RoleAPIBase &) = delete;
    RoleAPIBase(RoleAPIBase &&) noexcept;
    RoleAPIBase &operator=(RoleAPIBase &&) noexcept;

    // ── Host wiring (called once by role host after setup_infrastructure_) ────
    //
    // Note: set_short_tag + set_uid are GONE. Both are now ctor args and
    // immutable after construction. All remaining setters are for mutable
    // wiring state (infrastructure pointers, script paths, policies).

    /// Build the output-side queue (Tx). Constructs the appropriate queue
    /// implementation (ShmQueue::create_writer or ZmqQueue::push_to) from
    /// the options and stores it as a unique_ptr<QueueWriter> on Impl.
    /// @return true on success. On failure, no queue is wired.
    [[nodiscard]] bool build_tx_queue(const hub::TxQueueOptions &opts);

    /// Build the input-side queue (Rx). Constructs the appropriate queue
    /// implementation (ShmQueue::create_reader or ZmqQueue::pull_from) from
    /// the options and stores it as a unique_ptr<QueueReader> on Impl.
    [[nodiscard]] bool build_rx_queue(const hub::RxQueueOptions &opts);

    /// HEP-CORE-0036 §6.7 Option B — apply the broker's
    /// `CONSUMER_REG_ACK` payload to the Rx queue.  Per §6.7 Option B
    /// this is the SINGLE driver of Standby → Configured → Active:
    /// any bare set_* mutator args set in Standby are BUFFERED only;
    /// `apply_master_approval` merges them with REG_ACK fields and
    /// performs the transition in one step.
    ///
    /// Uniform across transports: dispatches through the polymorphic
    /// `QueueReader::apply_master_approval(ack)` which each concrete
    /// queue resolves to its transport-specific mutator (ZMQ extracts
    /// `producers[]`; SHM no-op when the secret was config-supplied).
    /// Then calls `start()` to perform the actual connect / attach
    /// (Configured → Active).  For Rx queues that are already Active
    /// (legacy single-step build + start), both calls are no-ops.
    ///
    /// Role hosts call this immediately after `register_consumer`
    /// returns success, before installing the heartbeat.  Returns
    /// false if the queue is not wired, the master-approval JSON is
    /// malformed (HEP §6.7 "fully refused"), or start() fails — all
    /// of which are fatal: the role host should treat any false as a
    /// startup-failed condition.
    [[nodiscard]] bool apply_consumer_reg_ack(const nlohmann::json &ack);

    /// HEP-CORE-0036 §6.7 Option B — apply the broker's `REG_ACK`
    /// payload to the Tx queue.  Producer-side mirror of
    /// `apply_consumer_reg_ack`: the same single-mutator Standby →
    /// Active driver, invoked at S3 after `register_producer_channel`
    /// returns success.
    ///
    /// Dispatches through `QueueWriter::apply_master_approval(ack)`.
    /// For ZMQ Tx queues, this extracts `ack["initial_allowlist"]`
    /// (Z85 pubkey strings per HEP-0036 §6.2), seeds the ZAP cache,
    /// then binds the PUSH socket and spawns the worker.  For SHM Tx,
    /// it is a no-op today (config-supplied secret applied at
    /// construction).
    ///
    /// Returns false if the queue is not wired, the JSON is malformed,
    /// or the activation fails — all of which are fatal: the role host
    /// must treat any false as a startup-failed condition.
    [[nodiscard]] bool apply_producer_reg_ack(const nlohmann::json &ack);

    /// HEP-CORE-0042 §5.5.3 — most-recent broker-echoed `instance_id`
    /// from PRODUCER_REG_ACK.  Zero means "no REG_ACK observed yet"
    /// (pre-registration or validate-only startup).  A nonzero value
    /// is guaranteed after any successful `apply_producer_reg_ack`
    /// (that function hard-errors on absent/zero per §5.2 "counter
    /// starts at 1").  Consumers of this value:
    ///   - Phase 3a.3 CHANNEL_AUTH_APPLIED_REQ emission — echoes
    ///     this value in the wire message so the broker's stale-
    ///     instance guard (§5.4 step a) can accept or reject.
    ///   - Observability / L3 tests — verifies REG_ACK capture without
    ///     needing to reach into pImpl internals.
    /// Overwritten on any subsequent REG_ACK — the field reflects
    /// LAST observed instance, not the FIRST or a channel-specific
    /// one.  Callers reading this concurrently with a live REG_ACK
    /// see either the pre- or post-value; no torn read (atomic
    /// uint64 with relaxed ordering).
    [[nodiscard]] std::uint64_t producer_instance_id() const noexcept;

    /// Reset metrics counters on the Tx/Rx queues. No-op if not wired.
    void reset_tx_queue_metrics();
    void reset_rx_queue_metrics();

    /// HEP-CORE-0036 §6.5 normative producer-side handler flow.
    /// For each `NotificationId::ChannelAuthChanged` entry in `msgs`,
    /// pull the broker's current channel allowlist via
    /// `BrokerRequestComm::get_channel_auth` (sync REQ/REP) and apply
    /// to the tx queue via `set_peer_allowlist`.  Consumed entries are
    /// removed from `msgs`; script dispatch never sees them (§I11 —
    /// auth synchronization is framework infrastructure, not a script
    /// callback).  Called from the worker thread cycle BEFORE
    /// `dispatch_notifications`.  No-op for roles without a tx queue
    /// (consumer-side; defensive — broker doesn't send notifies there).
    void handle_channel_auth_notifies(std::vector<pylabhub::scripting::IncomingMessage> &msgs);

    /// HEP-CORE-0036 §I11 + §6.5 — script-convenience snapshot of the
    /// producer-side allowlist for a channel.  Each entry is the
    /// `(role_uid, pubkey)` pair the broker emitted on the most recent
    /// `GET_CHANNEL_AUTH_ACK` for that channel.  Returns an empty
    /// vector if the role is not a producer of the channel or if no
    /// pull has completed yet.  Thread-safe; returns a copy under the
    /// internal lock so the caller may iterate without holding it.
    [[nodiscard]] std::vector<AllowedPeer> allowed_peers(const std::string &channel) const;

    /// Count of admitted peers for the named channel — convenience
    /// accessor equal to `allowed_peers(channel).size()`, provided for
    /// the framework's default loop-ready gate (HEP-CORE-0011
    /// §"Loop-ready gate") and for scripts that only need the cardinality
    /// without materialising the full snapshot.  Reads the same
    /// transport-agnostic, side-agnostic `allowlist_cache` seeded by
    /// `apply_{producer,consumer}_reg_ack` and grown by
    /// `handle_channel_auth_notifies` per HEP-CORE-0036 §I11.1.
    /// Thread-safe.
    [[nodiscard]] std::size_t admitted_peers_count(const std::string &channel) const;

    /// HEP-CORE-0011 §"Loop-ready gate" + HEP-CORE-0036 §I9.1 —
    /// queue-owned admission fact for the loop-ready gate.  Forwards
    /// to the queue that holds `channel` on this role
    /// (rx-side if present, else tx-side).  Layer-clean: no
    /// admission-cache snapshot copy, no vector allocation for a
    /// simple boolean question.  Used by `ConsumerCycleOps::default_init_ready`
    /// and `ProcessorCycleOps::default_init_ready`; script-facing
    /// observability keeps using `allowed_peers` / `admitted_peers_count`.
    [[nodiscard]] bool channel_admission_populated(const std::string &channel) const noexcept;

    /// HEP-CORE-0036 §I9.1 + §6.6.3 — topology-agnostic finalize step.
    ///
    /// Called by every role host UNIFORMLY (producer / consumer /
    /// processor) after `apply_master_approval`, once per channel.
    /// The API resolves this role's writer for the channel, seeds a
    /// `hub::PeerReadinessOracle` internally with the queue's own
    /// CURVE pubkey and the BRC forwarder, and calls
    /// `QueueWriter::finalize_connect`.  The queue decides whether
    /// to poll (fan-in DIALING PUSH — deferred connect) or no-op
    /// (every other topology / transport combination).
    ///
    /// Returns true on success or on legitimate no-op (queue not in
    /// deferred state, channel has no writer side on this role,
    /// SHM transport).  Returns false only on: permanent oracle
    /// error, cancellation via `is_cancelled`, timeout, or
    /// `start()` failure in the queue.
    ///
    /// `is_cancelled` is polled between broker RPCs and before every
    /// sleep so a shutdown during startup exits within one poll
    /// interval rather than burning `timeout_ms`.  Default (empty)
    /// disables cancellation.
    ///
    /// This method replaces the shipped-2026-07-11 pair
    /// `check_peer_ready` + `dial_now`, which are retired per §I9.1
    /// (topology and transport are queue-internal — role-side API
    /// carries topology-agnostic verbs only).
    bool finalize_channel_connect(const std::string &channel, std::uint64_t timeout_ms,
                                  const std::function<bool()> &is_cancelled = {}) noexcept;

    /// HEP-CORE-0028 §6a + HEP-CORE-0017 §3.3.2 + HEP-CORE-0007
    /// §CHANNEL_AUTH_CHANGED_NOTIFY (lines 1834-1838) —
    /// script-observable LIVE-peer role_uid lists for the named channel,
    /// backed by the broker's `phase=live` NOTIFY stream.  Symmetric
    /// with `consumer_count` / `producer_count`; same source
    /// (`live_peers[channel]` map), same population trigger (broker's
    /// first-heartbeat detection per HEP-CORE-0023 §2.5).
    /// - `producers(channel)` returns the role_uids of DIALING-side
    ///   producers currently past first-heartbeat.  Empty vector on
    ///   the producer side (only BINDING side receives live-peer
    ///   NOTIFYs) and on channels where no producer has fired
    ///   first-heartbeat yet.
    /// - `consumers(channel)` symmetric for consumers.
    /// Thread-safe; returns a copy under the internal lock.
    ///
    /// Return shape is `role_uid` strings only per HEP-CORE-0028 §6a.5
    /// — pubkeys are transport-level detail (they live on the queue
    /// via `curve_serverkey`, not on the script surface).  For the
    /// authorization-time allowlist snapshot (which DOES carry
    /// pubkeys) use `allowed_peers()` instead.
    [[nodiscard]] std::vector<std::string> producers(const std::string &channel) const;

    [[nodiscard]] std::vector<std::string> consumers(const std::string &channel) const;

    /// HEP-CORE-0028 §6a + HEP-CORE-0007 §CHANNEL_AUTH_CHANGED_NOTIFY
    /// (lines 1834-1838) — script-observable live-peer count for the
    /// named channel, backed by the broker's `phase=live` NOTIFY
    /// stream.  Counts the DIALING-side roles currently past first-
    /// heartbeat.  Returns 0 if no live peers, if the channel is
    /// unknown to this role, or if this role is itself the dialing
    /// side (only the BINDING side receives live-peer NOTIFYs).
    /// Thread-safe; snapshot under the internal lock.
    ///
    /// Use case: fan-out producer gates the first publish until at
    /// least one consumer is live (closes the libzmq PUB/SUB
    /// slow-joiner window).  Fan-in consumer gates admission logic
    /// until N producers are live.
    [[nodiscard]] std::size_t consumer_count(const std::string &channel) const;
    [[nodiscard]] std::size_t producer_count(const std::string &channel) const;

    /// HEP-CORE-0036 §6.7 (#190) — script-side state-machine query.
    /// True iff the queue serving the named channel is in the Active
    /// state (`start()` succeeded post-Configured gate).  Resolver:
    ///   - `channel == out_channel()` → tx side (processor-out specific)
    ///   - `channel == channel()` → primary side (tx for producer,
    ///     rx for consumer/processor-in)
    ///   - else → false
    /// Use case: scripts gate housekeeping or coordination logic on
    /// whether the channel's transport is usable, from callbacks
    /// OTHER than the channel's own data-loop callback (cycle ops
    /// already short-circuits the data-loop callback on Standby).
    /// Read-only; no script-side mutation.
    [[nodiscard]] bool is_channel_ready(const std::string &channel) const noexcept;

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

    /// HEP-CORE-0032 §8.6 strict-mode ABI reject on the role side.
    /// When true, on a broker REG_ACK / CONSUMER_REG_ACK carrying a
    /// MAJOR-axis ABI mismatch, the role refuses the Registered
    /// transition (register_producer / apply_consumer_reg_ack return
    /// false) instead of just logging.  Called once during setup by
    /// the role host from `config.startup().strict_abi_mismatch`.
    /// Task #327.
    void set_strict_abi_mismatch(bool v);
    bool strict_abi_mismatch() const;

    /// HEP-CORE-0041 §D4.5 mutual-auth opt-in (task #262).  When true,
    /// the consumer's SHM attach handshake in `apply_consumer_reg_ack`
    /// runs the 3-frame variant and requires the producer to prove
    /// possession of `producer_pubkey_z85`.  Called once during setup
    /// by the role host from `config.startup().shm_require_mutual_auth`.
    void set_shm_require_mutual_auth(bool v);
    bool shm_require_mutual_auth() const;

    /// HEP-CORE-0041 §D1(d) broker observer pubkey (task #317).  Stashed
    /// by producer's `apply_producer_reg_ack` from the
    /// `broker_observer_pubkey_z85` field on REG_ACK.  The producer's
    /// `AttachProtocolAcceptor` reads it (via an accessor callback set up
    /// by the role host) to verify observer handshakes from the broker
    /// — same crypto_box_open_easy pattern as the existing consumer path,
    /// but with this pubkey as the trust anchor instead of a channel
    /// allowlist entry.  Setter is safe to re-invoke on broker restart
    /// (broker generates fresh observer keypair per startup); acceptor
    /// picks up the new value on next handshake with no rewiring.
    void set_broker_observer_pubkey_z85(std::string pubkey_z85);
    /// Returns the currently-known broker observer pubkey (Z85, 40 chars),
    /// or empty string if no REG_ACK has delivered one yet.
    std::string broker_observer_pubkey_z85() const;

    /// **Reserved C++ extension point (as of 2026-05-20 — no callers in
    /// src/ or tests/).**  Full design in HEP-CORE-0019 §5.5; do not
    /// delete on dead-code sweeps — the consumer branches at
    /// `role_api_base.cpp:2122` and `:2189` are intentional and the
    /// hot-path wiring would be hard to reconstruct.  Future authors
    /// who install a caller should remove this "Reserved" tag in the
    /// same commit.
    ///
    /// Optional hook called at the end of `snapshot_metrics_json()` AND
    /// `snapshot_metrics_for_presence(role_type)` to let role hosts inject
    /// role-specific metrics fields into the JSON.  The hook is called
    /// once per snapshot — for a processor's per-presence emission it
    /// fires once for the consumer-presence payload and once for the
    /// producer-presence payload.  Hooks that inject side-specific data
    /// can inspect the existing `queue` / `role` keys to disambiguate.
    /// Default: no-op (null function).
    ///
    /// Intended caller: C++ role host (`producer_role_host.cpp` /
    /// `consumer_role_host.cpp` / `processor_role_host.cpp`) during
    /// `startup_()` BEFORE `start_handler_threads`.  Not script-callable
    /// (signature takes `std::function`).  For scalar metrics use
    /// `api.report_metric(key, value)` (HEP-CORE-0019 §5.1) — this hook
    /// is the structured-injection companion.
    void set_metrics_hook(std::function<void(nlohmann::json &)> hook);

    // ── FSM predicate (HEP-CORE-0036 §8.2 data-loop outer guard) ─────────────

    /// True iff at least one Presence on this role's handler is in
    /// `RegistrationState::Authorized` (HEP-CORE-0036 §4.3.2 — Layer 3
    /// data plane armed).  The data loop's outer guard reads this:
    /// when no presence is Authorized, the loop exits cleanly (single-
    /// hub case: presence lost authorization → role winds down; multi-
    /// hub case: keep running for any presence still authorized on
    /// other live hubs).  Atomic-only reads — safe from any thread,
    /// no lock needed.  Returns `false` if `handler_` is null
    /// (validate-only / test paths that bypass handler construction).
    [[nodiscard]] bool any_presence_authorized() const noexcept;

    // ── Identity ──────────────────────────────────────────────────────────────

    [[nodiscard]] const std::string &short_tag() const;
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
    /// Flag a critical (unrecoverable) error and request shutdown.
    ///
    /// Atomic combo (`RoleHostCore::set_critical_error`): sets
    /// `critical_error_=true`, `stop_reason_=CriticalError`, and
    /// `shutdown_requested_=true` in one operation.  Role exits
    /// with `stop_reason_string() == "critical_error"`.
    ///
    /// The framework emits a uniform ERROR-level log line
    /// `[short_tag/uid] CRITICAL: <msg>` BEFORE flipping state — so
    /// log scrapers see the message adjacent to the stop event.
    ///
    /// Audit S2 (2026-05-18) — `msg` is REQUIRED (no default).  All
    /// three engines (Python / Lua / Native C) enforce a non-empty
    /// message at the binding layer so operators ALWAYS get a
    /// breadcrumb explaining why a role flagged critical.  An empty
    /// string is technically allowed (skips the log line) but is
    /// considered a bug in the calling script — prefer
    /// `api.set_critical_error("brief reason")` over `api.set_critical_error("")`.
    /// Use for unrecoverable conditions (corrupt schema, hardware
    /// fault, etc.).  For ordinary stop, use `api.stop()`
    /// (reason = "normal").
    void set_critical_error(std::string_view msg);
    [[nodiscard]] bool critical_error() const;
    [[nodiscard]] std::string stop_reason() const;

    // ── Band pub/sub messaging (HEP-CORE-0030) ────────────────────────────────

    /// Join a named band. Auto-creates if it doesn't exist.
    [[nodiscard]] std::optional<nlohmann::json> band_join(const std::string &channel);

    /// Leave a band.  Returns the broker's response body (success or
    /// error per HEP-CORE-0007 §12.3) or `nullopt` on transport failure.
    [[nodiscard]] std::optional<nlohmann::json> band_leave(const std::string &channel);

    /// Send JSON message to all band members.
    void band_broadcast(const std::string &channel, const nlohmann::json &body);

    /// Query band member list.
    [[nodiscard]] std::optional<nlohmann::json> band_members(const std::string &channel);

    /// Local introspection: returns true iff the role-side
    /// `band_index_` currently has a routing entry for @p channel.
    /// This is the role's CACHED view of its own membership — it
    /// reflects the last successful `band_join` / `band_leave`
    /// outcome (and any `on_band_lost` clearing on hub-dead).
    /// HEP-CORE-0030 amendment 2026-05-19 (S4): scripts use this to
    /// branch on "should I attempt to join?" / "do I think I'm in?"
    /// without a broker round-trip.  For authoritative
    /// broker-side membership use `band_members(channel)`.
    [[nodiscard]] bool is_in_band(const std::string &channel) const noexcept;

    // ── Inbox client management ───────────────────────────────────────────────

    struct InboxOpenResult
    {
        std::shared_ptr<hub::InboxClient> client;
        hub::SchemaSpec spec;
        std::string packing;
        size_t item_size{0};
    };

    [[nodiscard]] std::optional<InboxOpenResult> open_inbox_client(const std::string &target_uid);
    [[nodiscard]] bool wait_for_role(const std::string &uid, int timeout_ms = 5000);

    // ── Output side (safe defaults when no output wired) ──────────────────────
    //
    // Flat data-plane verbs. Return nullptr / no-op when the role has no
    // output side. Callers who have configured an output side will never
    // see nullptr except when the underlying queue genuinely has no slot
    // available within the timeout. See loop_design_unified.md for timing.

    /// HEP-CORE-0036 §6.7 state-machine accessors (#189, Stage 1B).
    /// `is_tx_active()` returns true iff the output queue is in the
    /// Active state — `start()` succeeded and `stop()` has not run.
    /// Cycle ops gate `write_acquire` + script `on_step`/`on_produce`
    /// dispatch on this so a Standby/Configured queue does NOT fire
    /// the user callback and does NOT increment drop counters (§6.7
    /// "Standby skip is a lifecycle condition, not a runtime drop").
    /// `is_rx_active()` is the symmetric input-side accessor.  Both
    /// return false when the queue has not been built yet.
    [[nodiscard]] bool is_tx_active() const noexcept;
    [[nodiscard]] bool is_rx_active() const noexcept;

    [[nodiscard]] void *write_acquire(std::chrono::milliseconds timeout) noexcept;
    void write_commit() noexcept;
    void write_discard() noexcept;
    /// Flexzone pointer for the given side (Tx or Rx). Single region per
    /// channel, fully read+write on both endpoints per HEP-CORE-0002 §2.2.
    /// Returns nullptr when the side is not wired or the channel has no
    /// flexzone configured.
    [[nodiscard]] void *flexzone(ChannelSide side);
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
    void read_release() noexcept;
    [[nodiscard]] size_t read_item_size() const noexcept;
    [[nodiscard]] uint64_t in_slots_received() const;
    [[nodiscard]] uint64_t last_seq() const;
    [[nodiscard]] size_t in_capacity() const;
    [[nodiscard]] std::string in_policy() const;
    void set_verify_checksum(bool enable);

    // ── Diagnostics ───────────────────────────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const;
    [[nodiscard]] uint64_t loop_overrun_count() const;
    [[nodiscard]] uint64_t last_cycle_work_us() const;

    // ── Schema sizes ──────────────────────────────────────────────────────────
    //
    // Two distinct quantities per side, both derived from the same fz_spec:
    //   - logical:  compute_schema_size(spec, packing) — the C struct size
    //               scripts see.  Use for byte arithmetic on FlexFrame objects.
    //   - physical: align_to_physical_page(logical) — the SHM region size
    //               (page-aligned).  Use for SHM span / mmap reasoning.
    // Both are populated together by the frame at setup; both readable here.

    [[nodiscard]] size_t slot_logical_size(std::optional<ChannelSide> side = std::nullopt) const;
    [[nodiscard]] size_t
    flexzone_logical_size(std::optional<ChannelSide> side = std::nullopt) const;
    [[nodiscard]] size_t
    flexzone_physical_size(std::optional<ChannelSide> side = std::nullopt) const;

    /// Flexzone presence check per side.  Reads from this object's
    /// FlexzoneInfoCache, which the frame populates at setup time
    /// via `set_flexzone_info_cache_()`.
    /// PRE: setup_infrastructure_ has completed (caller is in script
    /// context, which only runs after step 5 invoke_on_init).
    [[nodiscard]] bool has_tx_fz() const noexcept;
    [[nodiscard]] bool has_rx_fz() const noexcept;

    // ── Flexzone introspection cache (framework-internal) ────────────────────
    //
    // Populated exactly once per role lifetime by
    // `RoleHostFrame::setup_infrastructure_` at step 2b, AFTER
    // build_*_queue succeeds.  The cache stores derived SCALARS
    // (sizes + has flags) — NOT a copy of the full SchemaSpec, which
    // lives only in `Presence::fz_spec` on the frame.
    //
    // Invariant: per side, physical_size == align_to_physical_page(logical_size)
    // when has_*_fz is true; both are 0 otherwise.  Checked at engine setup
    // entry (engine_module_params.cpp::engine_lifecycle_startup).
    //
    // Linear forward-time API sequence (per docs/tech_draft/
    // docs/archive/transient-2026-06-02/role_host_template_design.md §11.6.2):
    //   - setter: called once at step 2b by the frame.
    //   - readers (`has_*_fz()`, `flexzone_*_size()`, `fz_info_cache()`):
    //     called by scripts + framework code at step 5+.  All readers are
    //     after the single write.
    struct FlexzoneInfoCache
    {
        // TX side
        bool has_tx_fz{false};
        size_t tx_logical_size{0};  ///< compute_schema_size(spec, packing)
        size_t tx_physical_size{0}; ///< align_to_physical_page(tx_logical_size)

        // RX side
        bool has_rx_fz{false};
        size_t rx_logical_size{0};
        size_t rx_physical_size{0};
    };

    /// Framework-internal setter (trailing underscore: not for script
    /// callers).  Frame populates this exactly once after build_*_queue.
    void set_flexzone_info_cache_(const FlexzoneInfoCache &cache) noexcept;

    /// Read-accessor for the whole cache (framework + assertion use).
    [[nodiscard]] const FlexzoneInfoCache &fz_info_cache() const noexcept;

    // ── Spinlocks (delegates to whichever side has SHM) ───────────────────────

    [[nodiscard]] hub::SharedSpinLock get_spinlock(size_t index,
                                                   std::optional<ChannelSide> side = std::nullopt);
    [[nodiscard]] uint32_t spinlock_count(std::optional<ChannelSide> side = std::nullopt) const;

    // ── Custom metrics ────────────────────────────────────────────────────────

    void report_metric(const std::string &key, double value);
    void report_metrics(const std::unordered_map<std::string, double> &kv);
    void clear_custom_metrics();

    // ── Metrics snapshot (data-driven, no virtual) ────────────────────────────

    /// Role-wide aggregate snapshot — used by script-facing API surfaces
    /// (`Producer/Consumer/ProcessorAPI::snapshot_metrics_json()`) and by
    /// any caller that wants the full role-wide view.  Combines both
    /// directions of a processor into a single JSON (`in_queue` +
    /// `out_queue` keys when both sides are bound).
    [[nodiscard]] nlohmann::json snapshot_metrics_json() const;

    /// Per-presence emission snapshot per HEP-CORE-0019 §2.3 Phase 6.
    /// Returns metrics shaped for the `role_type` emission only:
    ///   - `queue`: rx_queue metrics for `"consumer"`, tx_queue metrics
    ///     for `"producer"`.  Single key (no `in_queue`/`out_queue`
    ///     split) — the broker keys the row by `(channel, uid,
    ///     role_type)` so the direction is already implicit.
    ///   - `role`: side-relevant counters only (consumer →
    ///     `in_slots_received`; producer → `out_slots_written`,
    ///     `out_drop_count`).  `script_error_count` appears on both
    ///     (direction-agnostic).
    ///   - `loop`, `inbox`, `custom`: role-wide; appear on every
    ///     presence (one role, one loop / inbox / custom map).
    /// The `metrics_hook` fires once per call — for a processor's
    /// 2-presence emission it fires once per side.
    /// Used by `on_heartbeat_tick_` to populate per-presence heartbeats.
    [[nodiscard]] nlohmann::json snapshot_metrics_for_presence(const std::string &role_type) const;

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
    /// the RoleAPIBase ctor alongside short_tag/uid. All role-scope
    /// threads (worker / ctrl / inbox drainer / future) live under this
    /// one manager — same dynamic lifecycle module
    /// "ThreadManager:{short_tag}:{uid}", same bounded-join, same
    /// process-wide leak aggregator. Usage:
    ///
    ///   api_->thread_manager().spawn("worker",
    ///       [&] { scripting::ThreadEngineGuard g(*engine_); worker_main_(); });
    ///
    /// No throw path — invariant "manager is always ready" enforced by
    /// the ctor signature (short_tag + uid are required positional args).
    [[nodiscard]] pylabhub::utils::ThreadManager &thread_manager();

    // ── Handler-mode control plane (Wave-B M4c, sole path after M4f) ────────
    //
    // Spawns one ctrl thread per `RoleHandler::connections()` entry.
    // First thread is master (HEP-CORE-0031 §4.2.1), the rest are peers.
    // Single-shot per RoleAPIBase instance — a second call is refused
    // with a WARN log.
    //
    // Pre-M4f, the legacy `start_ctrl_thread` / `set_broker_comm` /
    // `pImpl->broker_channel` fallback view co-existed during the M5/
    // M6/M7 role-host migration window.  M4f deleted those entirely
    // once every production role host (producer/consumer/processor)
    // moved to `start_handler_threads`.  Class A/B/D routing helpers
    // (`resolve_bc_for_channel/role/band`) now go through handler-mode
    // only; no fallback path remains.

    /// Take ownership of `handler`, connect each `HubConnection`'s
    /// BRC, install per-BRC notification + hub-dead callbacks, and
    /// spawn one ctrl thread per connection (first = master).
    ///
    /// Atomicity: refuses (returns false + WARN log) if ctrl threads
    /// are already running on this RoleAPIBase.  Single-shot per
    /// instance.
    ///
    /// Diagnostics: emits per-step INFO logs so the spawn sequence
    /// across N threads is observable in test + production logs.
    /// HEP-CORE-0040 §172: the role CURVE identity must already be
    /// in `secure().keys()` under `"role_identity"` before this method
    /// runs (handler's BRCs read it on-site via `secure().keys()`).
    [[nodiscard]] bool start_handler_threads(std::unique_ptr<RoleHandler> handler);

    /// Symmetric teardown: signal each BRC's poll loop, drain the
    /// ThreadManager via the §4.1 bracket contract, disconnect +
    /// release BRCs, then release the handler.  Idempotent + `noexcept`.
    ///
    /// Thread context: MUST be called from the owning thread (the
    /// role-host worker thread driving teardown, or the
    /// construction context if the role was never started end-to-end).
    /// Concurrent invocation with other readers of `handler()` is
    /// undefined.  The production teardown sequence calls this from
    /// the worker thread inside `do_role_teardown`, where no
    /// concurrent reader exists.
    void stop_handler_threads() noexcept;

    /// Non-destructive "signal ctrl threads to exit poll loop" — the
    /// signal used by `do_role_teardown` Step 12.  Calls `brc->stop()`
    /// on every connection's BRC so all N handler_ctrl_* threads
    /// observe the stop flag in their next poll iteration.  No-op
    /// when no handler is attached (validate-only run, or after
    /// `stop_handler_threads()`).
    ///
    /// Does NOT join threads, disconnect sockets, or release BRCs —
    /// that's `stop_handler_threads()`.  Safe to call multiple times;
    /// safe to call before any thread is spawned (it's the "make
    /// sure no poll loop is wedged" signal).
    void stop_ctrl_for_teardown() noexcept;

    /// Read-only access to the handler (for migration sites that
    /// need to call `handler.brc_for_*` or `find_presence_for_*`).
    /// Returns nullptr if `start_handler_threads` has not been
    /// called (or after `stop_handler_threads`).
    ///
    /// Pointer lifetime: VALID between the return of a successful
    /// `start_handler_threads` and the call to `stop_handler_threads`.
    /// Callers MUST NOT cache this pointer across a teardown
    /// boundary — `stop_handler_threads` resets the unique_ptr,
    /// invalidating any external copies.  Re-acquire via `handler()`
    /// on every entry into a method that might run after teardown.
    [[nodiscard]] RoleHandler *handler() const noexcept;

    /// Per-connection liveness — true if connection `i` has not
    /// observed a ZMQ_EVENT_DISCONNECTED since `start_handler_threads`.
    /// Returns false for out-of-range `i` (`i >= handler()->connections().size()`)
    /// and before `start_handler_threads`/after `stop_handler_threads`.
    ///
    /// Policy (A2 — Wave-B M8 prep): each connection's `on_hub_dead`
    /// callback marks its own slot via this bitmask.  Master (i==0)
    /// death additionally triggers role-wide shutdown
    /// (`set_stop_reason(HubDead) + request_stop`); peer (i>0) deaths
    /// log WARN and update the bitmask only — the role keeps running
    /// on its master so partial work continues until the master also
    /// dies OR the role host policy explicitly decides to exit on
    /// peer loss.  See HEP-CORE-0023 §2.5 "Multi-hub failure policy."
    ///
    /// No reconnect tracking: once a connection is marked dead it
    /// stays dead until the role exits.  ZMQ DEALER sockets do
    /// auto-reconnect at the transport layer, but BRC only fires
    /// hub-dead, not hub-resurrected.  Adding resurrection signal
    /// is a separate task.
    [[nodiscard]] bool is_connection_alive(std::size_t i) const noexcept;

    /// Count of currently-alive connections (read of the same bitmask
    /// `is_connection_alive` consults).  Useful for role-host policy
    /// decisions like "exit if 0 connections alive."
    [[nodiscard]] std::size_t connections_alive_count() const noexcept;

    /// Negotiate the heartbeat cadence with the hub + install the
    /// periodic tick on the master ctrl thread.  Called from the
    /// role host after `start_handler_threads` + register_*_channel:
    ///     api.start_handler_threads(handler);
    ///     auto reg = api.register_producer_channel(opts);
    ///     api.install_heartbeat(cfg_ms,
    ///                            extract_hub_heartbeat_max(reg));
    ///
    /// Cadence policy (HEP-CORE-0023 §2.5): effective_ms =
    /// `min(role_cfg_ms, hub_max_ms)`.  If role exceeds hub's max, a
    /// WARN log fires and the effective interval is downgraded to the
    /// hub max so the role doesn't get reaped by hub-side liveness.
    /// If `hub_max_ms` is nullopt (hub didn't advertise a max), the
    /// role's configured cadence is used as-is.
    ///
    /// Routing: schedules the tick on the FIRST connection's BRC
    /// (the master).  The tick body (`on_heartbeat_tick_`) routes
    /// each per-presence heartbeat per-channel via
    /// `pImpl->resolve_bc_for_channel`, so the choice of scheduling
    /// BRC is just "which thread runs the timer" — the actual
    /// heartbeat wire frames go out on the correct BRC per presence
    /// (dual-hub processor → 2 BRCs, single-hub → 1 BRC).
    ///
    /// Idempotent within a single ctrl-thread lifetime; calling twice
    /// reinstalls the periodic task on the same slot (no leak).  Must
    /// be called AFTER `start_handler_threads`.
    ///
    /// @param role_cfg_ms  Role's preferred cadence (from config).
    /// @param hub_max_ms_opt Hub's tolerated max (parsed from REG_ACK
    ///                       `heartbeat.heartbeat_interval_ms`); pass
    ///                       `std::nullopt` if not advertised.
    void install_heartbeat(int role_cfg_ms, std::optional<int> hub_max_ms_opt) noexcept;

    /// Extract the hub's advertised heartbeat-tolerated-max from a
    /// REG_ACK body (or any reply that carries it).  REG_ACK shape
    /// per HEP-CORE-0023 §2.5: `{..., "heartbeat": {"heartbeat_interval_ms": N}}`.
    /// Returns `std::nullopt` if the field is missing or malformed —
    /// caller then passes nullopt to `install_heartbeat` and the role
    /// uses its configured cadence as-is.  Static so role hosts can
    /// call it on a captured REG_ACK without an instance.
    [[nodiscard]] static std::optional<int>
    extract_hub_heartbeat_max(const nlohmann::json &reg_ack_body) noexcept;

    /// Append inbox metadata (endpoint + schema_json + packing + checksum)
    /// to a REG_REQ / CONSUMER_REG_REQ payload.  No-op if the role has
    /// no inbox configured (`inbox_cfg.has_inbox() == false`) OR if no
    /// inbox queue has been wired via `set_inbox_queue` (handler-mode
    /// role hosts MUST call `set_inbox_queue` before this method).
    ///
    /// Role hosts call this between `build_*_reg_payload` and
    /// `register_*_channel`:
    ///
    ///     auto reg = hub::build_producer_reg_payload(...);
    ///     api.append_inbox_to_reg(reg, inbox_cfg);
    ///     auto result = api.register_producer_channel(reg);
    void append_inbox_to_reg(nlohmann::json &opts, const config::InboxConfig &inbox_cfg) const;

    /// Explicitly deregister from broker while the ctrl threads are still
    /// running to process the command — call BEFORE `stop_handler_threads()`
    /// (which signals ctrl threads to exit; pending DEREG RPCs would
    /// then hang waiting for an ACK that no thread will deliver).
    /// Sends DEREG_REQ and/or CONSUMER_DEREG_REQ for whatever was recorded
    /// in `shared.{producer,consumer}_channel` by the matching register_*
    /// calls.
    void deregister_from_broker();

    // ── Broker protocol helpers (require ctrl thread running) ────────────

    /// Register a producer channel (REG_REQ → REG_ACK).
    [[nodiscard]] std::optional<nlohmann::json>
    register_producer_channel(const nlohmann::json &opts, int timeout_ms = 5000);

    /// Discover a channel (DISC_REQ → DISC_ACK).
    [[nodiscard]] std::optional<nlohmann::json> discover_channel(const std::string &channel,
                                                                 int timeout_ms = 10000);

    /// Register as consumer (CONSUMER_REG_REQ → CONSUMER_REG_ACK).
    [[nodiscard]] std::optional<nlohmann::json> register_consumer(const nlohmann::json &opts,
                                                                  int timeout_ms = 5000);

    /// Deregister a producer channel (DEREG_REQ → DEREG_ACK or ERROR).
    /// Returns the broker's response body (success or error per HEP-CORE-0007
    /// §12.3) or `nullopt` on transport failure.
    [[nodiscard]] std::optional<nlohmann::json>
    deregister_producer_channel(const std::string &channel, int timeout_ms = 5000);

    /// Deregister a consumer (CONSUMER_DEREG_REQ → CONSUMER_DEREG_ACK or ERROR).
    /// Returns the broker's response body (success or error per HEP-CORE-0007
    /// §12.3) or `nullopt` on transport failure.
    [[nodiscard]] std::optional<nlohmann::json> deregister_consumer(const std::string &channel,
                                                                    int timeout_ms = 5000);

    /// HEP-CORE-0041 §9 D4 broker pre-confirm: producer-side wrapper
    /// for `BrokerRequestComm::consumer_attach(...)`.  Routes the call
    /// through the channel-bound BRC.  Used by the producer's
    /// `ShmAttachOrchestrator::BrokerQuery` callback at attach time.
    /// Returns the broker's reply body
    /// (`{status: "success" | "denied", ...}`) or `nullopt` on
    /// transport failure / timeout / no-BRC-for-channel.
    ///
    /// **FRAMEWORK-INTERNAL — DO NOT BIND TO SCRIPTS.**  This method
    /// triggers a broker authorization query for an arbitrary
    /// (channel, consumer_pubkey) pair.  Exposing it to scripts
    /// would let user code probe the broker for arbitrary peer
    /// authorization decisions outside the auth flow — an
    /// information-disclosure surface that bypasses the
    /// orchestrator's same-process attach gating.
    ///
    /// Following the established RoleAPIBase convention: many
    /// methods (`register_producer_channel`, `start_handler_threads`,
    /// `apply_producer_reg_ack`, `build_tx_queue`, `set_channel`, …)
    /// are C++-public for the role host's worker_main_ + RoleHostFrame
    /// to invoke, but are NEVER added to `push_closure(...)` in
    /// `src/scripting/lua_engine.cpp` or `.def(...)` in
    /// `src/scripting/python_engine.cpp` or registered with the
    /// `native_engine_api.h` C ABI.  Script-visible accessors are
    /// the observation-only set: `uid`, `name`, `channel`,
    /// `allowed_peers`, `producers`, `is_channel_ready`,
    /// `queue_mechanism`, `metrics`, geometry queries, etc.
    ///
    /// Audit pin: zero hits for `consumer_attach` in
    /// `src/scripting/` as of 2026-06-22 (verified during #269
    /// pre-REVIEW-A scope review).  Engine-binding authors MUST
    /// preserve this exclusion in any future binding-parity sweep.
    [[nodiscard]] std::optional<nlohmann::json>
    consumer_attach(const std::string &channel, const std::string &consumer_pubkey,
                    const std::string &consumer_role_uid, const std::string &producer_role_uid,
                    int timeout_ms = 5000);

    // ── Inbox drain ─────────────────────────────────────────────────────────
    //
    // Drains all pending inbox messages and invokes engine->invoke_on_inbox()
    // for each. Also called by the data loop (Step C) before each cycle's
    // invoke. Requires set_engine() to have been called.
    void drain_inbox_sync();

    // ── Infrastructure access (for engine binding layers) ─────────────────────

    [[nodiscard]] RoleHostCore *core() const;
    [[nodiscard]] hub::InboxQueue *inbox_queue() const;

    /// Side-presence checks for engines/callers that need to gate script-facing
    /// method registration on which side is wired. Supersedes the producer()/
    /// consumer() pointer accessors above.
    [[nodiscard]] bool has_tx_side() const noexcept; ///< True iff Tx queue wired.
    [[nodiscard]] bool has_rx_side() const noexcept; ///< True iff Rx queue wired.

    /// Queue metrics for the given side. Empty QueueMetrics when the side
    /// is not wired. Routes to QueueReader::metrics() / QueueWriter::metrics().
    [[nodiscard]] hub::QueueMetrics queue_metrics(ChannelSide side) const noexcept;

    /// Negotiated transport-level CURVE mechanism of the given side's
    /// queue (HEP-CORE-0035 §2 + AUTH_TODO §C5, #161).  Returns
    /// `Mechanism::Uninitialized` when the side is not wired OR the
    /// queue is not started OR the underlying transport is SHM (which
    /// does not negotiate a libzmq mechanism).  Returns
    /// `Mechanism::Curve` whenever the side has a started ZmqQueue
    /// (post-C4 the public ZmqQueue factories are CURVE-only and the
    /// `ZmqQueue::start()` guard enforces ZMQ_CURVE).
    ///
    /// **Intended uses.**  Scripts asserting CURVE engagement before
    /// sending sensitive data (`assert api.queue_mechanism(...) ==
    /// Mechanism.Curve`); telemetry tagging queue health; cross-binding
    /// confirmation that the lib-level CURVE invariant is reachable
    /// from the script layer (anti-recursion observability for #161
    /// C5).
    [[nodiscard]] hub::Mechanism queue_mechanism(ChannelSide side) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;

#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
    // ── L2 test access (friend) ────────────────────────────────────────────
    //
    // Private setter for `pImpl->handler_`.  Reachable ONLY through the
    // `test::RoleAPIBaseTestAccess` friend declaration above; production
    // code paths (start_handler_threads etc.) use their own internal
    // moves into the same slot.  This method does NOT bypass any
    // protocol check — it just installs a handler the test constructed.
    // The §8.2 outer guard runs unchanged afterward, scanning the
    // installed handler's presences for `Authorized` state.
    //
    // Double-gated PHYSICALLY ABSENT in Release / non-test builds.
    void install_handler_for_test_(std::unique_ptr<RoleHandler> handler);
#endif

    // ── Ctrl thread private helpers (called from within the ctrl thread) ──
    // Access pImpl members directly — no bare pointers cross thread boundary.

    void on_heartbeat_tick_();
    // M1.4 (2026-05-11): `on_metrics_report_tick_` deleted; metrics
    // piggyback on heartbeat per HEP-CORE-0019 §2.3 Phase 6.

    /// HEP-CORE-0041 1i-mig-4 (#272) — SHM-branch dispatch for
    /// `apply_consumer_reg_ack`.  Runs the §5.5 ZAP-CURVE dial against
    /// the producer's `shm_capability_endpoint` (D3: bounded retry on
    /// ECONNREFUSED to absorb the H3a race window where REG_ACK racing
    /// the producer's listener-bind), receives the SHM memfd via
    /// SCM_RIGHTS, hands the fd to the rx queue, and starts it.
    ///
    /// On success: `pImpl->shm_consumer` owns the SHM consumer (D1),
    /// the rx queue is Active.  Returns true.
    ///
    /// On failure (timeout, transport error, queue refusal, post-retry
    /// ECONNREFUSED): logs the cause and returns false.  Caller treats
    /// false as a fatal registration failure (matches the §6.7 "fully
    /// refused" semantics on the ZMQ path).
    [[nodiscard]] bool apply_consumer_reg_ack_shm_(const std::string &channel_name,
                                                   const std::string &shm_endpoint,
                                                   const std::string &producer_pubkey_z85);
};

} // namespace pylabhub::scripting
