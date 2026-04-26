#pragma once
/**
 * @file hub_state.hpp
 * @brief HubState — read-mostly aggregate of the hub's authoritative state.
 *
 * HEP-CORE-0033 §8; ratified in docs/tech_draft/HUB_CHARACTER_PREREQUISITES.md
 * §G2 ("broker as single mutator" model).
 *
 * Phase G2.1 (skeleton, this commit)
 * ----------------------------------
 * The class, its entry types, and its event subscription API compile and link
 * but are NOT yet wired into BrokerService. Mutators exist as `friend`-access
 * private methods; they update the private maps and fire subscribed handlers,
 * but broker handlers still write into their own private maps. G2.2 flips
 * that around: BrokerService's private maps become `HubState&`, and each
 * broker inbound handler calls a HubState mutator instead of mutating
 * ad-hoc.
 *
 * Ownership
 * ---------
 * HubHost owns the single HubState. BrokerService holds a `HubState&`
 * (plumbed during G2.2) and is the only caller of the private `_set_*`
 * mutators — enforced by `friend class broker::BrokerService`. Scripts
 * and admin reach state via HubAPI / AdminService, which hold:
 *   - `const HubState&` for reads (via public snapshot accessors), and
 *   - `BrokerService&` for mutations (which re-enter HubState through
 *     the friend path after authorization).
 *
 * Thread model
 * ------------
 * `snapshot()` and per-entry lookups take a shared (reader) lock over the
 * state maps; mutators take the unique (writer) lock. Event dispatch
 * happens outside the state lock — each mutator copies the handler list
 * under a short handlers-lock and invokes handlers without holding either
 * lock, so a handler is free to subscribe/unsubscribe or read state.
 *
 * Strategy A (HEP-0033 §G2.1): entry types here are fresh definitions in
 * `pylabhub::hub`, duplicating today's broker-internal `ChannelEntry`
 * and `Band` field sets for the duration of G2.1. G2.2 removes the
 * broker duplicates when BrokerService's maps are absorbed into HubState.
 */

#include "pylabhub_utils_export.h"
#include "utils/channel_pattern.hpp"
#include "utils/json_fwd.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pylabhub::broker
{
class BrokerService;
class BrokerServiceImpl; ///< pImpl; grants the impl class friend access too.
}

namespace pylabhub::hub
{

// ─── Enums ──────────────────────────────────────────────────────────────────

enum class ChannelStatus
{
    PendingReady, ///< Registered but first heartbeat not yet received.
    Ready,        ///< Producer has sent ≥1 heartbeat; consumer discover allowed.
    Closing,      ///< Marked for removal (heartbeat timeout or explicit close).
};

enum class RoleState
{
    Connected,    ///< Role has registered; heartbeats flowing.
    Disconnected, ///< Role lost; still in map within the grace window.
                  ///< Past grace, the entry is removed from `HubState::roles`
                  ///< entirely (no persisted "evicted" state — see HEP-0033 §8).
};

enum class PeerState
{
    Connecting,
    Connected,
    Disconnected,
};

/// Why a channel is being closed (parameter to `_on_channel_closed`).
/// The specific reason drives which CHANNEL_*_NOTIFY the broker emits;
/// HubState itself stores no reason, but subscribers may care.
enum class ChannelCloseReason
{
    VoluntaryDereg,   ///< DEREG_REQ from the producer (clean shutdown).
    HeartbeatTimeout, ///< Heartbeat-sweep reclaimed a stuck channel.
    AdminClose,       ///< Explicit close via script or admin RPC.
    BrokerShutdown,   ///< Broker process is stopping.
};

PYLABHUB_UTILS_EXPORT const char *to_string(ChannelStatus      s) noexcept;
PYLABHUB_UTILS_EXPORT const char *to_string(RoleState          s) noexcept;
PYLABHUB_UTILS_EXPORT const char *to_string(PeerState          s) noexcept;
PYLABHUB_UTILS_EXPORT const char *to_string(ChannelCloseReason r) noexcept;

// ─── Entry types (HEP-CORE-0033 §8) ─────────────────────────────────────────

/// Consumer attached to a channel.
struct ConsumerEntry
{
    uint64_t    consumer_pid{0};
    std::string consumer_hostname;
    std::string zmq_identity; ///< ROUTER identity for direct notifications.

    std::string role_name;
    std::string role_uid;

    // Inbox (HEP-CORE-0027).
    std::string inbox_endpoint;
    std::string inbox_schema_json;
    std::string inbox_packing;  ///< "aligned" | "packed"
    std::string inbox_checksum; ///< "enforced" | "manual" | "none"

    std::chrono::system_clock::time_point connected_at{
        std::chrono::system_clock::now()};
};

/// Channel registered with the broker.
struct ChannelEntry
{
    std::string name;          ///< Key in HubState::channels.
    std::string shm_name;
    std::string schema_hash;   ///< Hex (64 chars).
    uint32_t    schema_version{0};
    std::string schema_id;     ///< Named-schema id; empty = anonymous.
    std::string schema_blds;

    uint64_t    producer_pid{0};
    std::string producer_hostname;
    std::string producer_role_name;
    std::string producer_role_uid;
    std::string producer_zmq_identity;

    nlohmann::json             metadata;
    std::vector<ConsumerEntry> consumers;

    ChannelStatus                         status{ChannelStatus::PendingReady};
    std::chrono::steady_clock::time_point last_heartbeat{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point state_since{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point closing_deadline{};

    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    zmq_ctrl_endpoint;
    std::string    zmq_data_endpoint;
    std::string    zmq_pubkey;
    std::string    data_transport{"shm"};
    std::string    zmq_node_endpoint;

    std::string    inbox_endpoint;
    std::string    inbox_schema_json;
    std::string    inbox_packing;
    std::string    inbox_checksum;

    std::chrono::system_clock::time_point created_at{
        std::chrono::system_clock::now()};
};

/// A single band member.
struct BandMember
{
    std::string role_uid;
    std::string role_name;
    std::string zmq_identity;
    std::chrono::steady_clock::time_point joined_at{
        std::chrono::steady_clock::now()};
};

/// Named messaging band (HEP-CORE-0030).
struct BandEntry
{
    std::string             name;
    std::vector<BandMember> members;
    std::chrono::steady_clock::time_point created_at{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_activity{
        std::chrono::steady_clock::now()};
};

/// Registered role (plh_role process). Independent of channel membership.
struct RoleEntry
{
    std::string uid;       ///< Key in HubState::roles.
    std::string name;
    std::string role_tag;  ///< "prod" / "cons" / "proc" or a custom tag.
    std::vector<std::string> channels;
    RoleState   state{RoleState::Connected};

    std::chrono::system_clock::time_point first_seen{
        std::chrono::system_clock::now()};
    std::chrono::steady_clock::time_point last_heartbeat{
        std::chrono::steady_clock::now()};

    nlohmann::json latest_metrics;
    std::chrono::system_clock::time_point metrics_collected_at{};

    std::string pubkey_z85; ///< Role's CurveZMQ public key (Z85, 40 chars).
};

/// Federation peer (direct-connected hub, HEP-CORE-0022).
///
/// Peer uids follow the same `tag.name.unique` structure as role uids
/// but with tag==`hub` (HEP-0033 §G2.2.0b). Example: `hub.lab1.pid42`.
struct PeerEntry
{
    std::string uid;      ///< Peer hub UID; key in HubState::peers. Must validate as PeerUid.
    std::string endpoint;
    PeerState   state{PeerState::Connecting};
    std::chrono::steady_clock::time_point last_seen{
        std::chrono::steady_clock::now()};
    std::string pubkey_z85;
    std::vector<std::string> relay_channels; ///< Channels we relay TO this peer.

    /// Raw ROUTER routing identity captured when the peer's DEALER first
    /// contacted our broker (inbound peers only; empty for outbound-only
    /// peers whose connection we own). Required so the broker can address
    /// send_to_identity() calls (relay NOTIFY, HUB_TARGETED_MSG, etc.)
    /// back to this peer without maintaining a shadow map.
    std::string zmq_identity;
};

/// SHM block registered for a channel — pointer-to-collect (HEP-0033 §9.2).
struct ShmBlockRef
{
    std::string channel_name;
    std::string block_path;
};

/// Broker-internal counters (HEP-CORE-0023 §2.5 + general instrumentation).
struct BrokerCounters
{
    // Role state transitions (HEP-CORE-0023 §2.5).
    uint64_t ready_to_pending_total{0};
    uint64_t pending_to_deregistered_total{0};
    uint64_t pending_to_ready_total{0};

    // Loop instrumentation.
    uint64_t bytes_in_total{0};
    uint64_t bytes_out_total{0};   // Always 0 today — multi-target fan-out
                                   // makes per-message accounting ambiguous;
                                   // see HEP-CORE-0033 §9.4.

    // Per-message-type counts (kept opaque to stay extensible).
    // `msg_type_counts[type]` bumps for every dispatch-completed message
    // of a known msg_type (success OR error).  Unknown msg_types are NOT
    // inserted here — they only bump `msg_type_counts["sys.unknown_msg_type"]`
    // (cardinality-attack mitigation per HEP-0033 §9.3 R1).
    // `msg_type_counts[type]_errors` (suffix convention) is unused; per-type
    // errors live in `msg_type_errors` below.
    std::unordered_map<std::string, uint64_t> msg_type_counts;

    /// Per-msg-type error subset of `msg_type_counts` — bumped at the same
    /// dispatcher post-processing step when the handler hit an exception or
    /// validation rejection.  Per HEP-CORE-0033 §9.4.
    std::unordered_map<std::string, uint64_t> msg_type_errors;
};

/// Point-in-time aggregate returned by HubState::snapshot().
struct HubStateSnapshot
{
    std::unordered_map<std::string, ChannelEntry> channels;
    std::unordered_map<std::string, RoleEntry>    roles;
    std::unordered_map<std::string, BandEntry>    bands;
    std::unordered_map<std::string, PeerEntry>    peers;
    std::unordered_map<std::string, ShmBlockRef>  shm_blocks;
    BrokerCounters                                counters;
    std::chrono::system_clock::time_point         captured_at{
        std::chrono::system_clock::now()};
};

// ─── Event subscription ─────────────────────────────────────────────────────

using HandlerId = uint64_t;
inline constexpr HandlerId kInvalidHandlerId = 0;

// ─── Friend forward-decls ───────────────────────────────────────────────────

namespace test { struct HubStateTestAccess; } // test-only friend shim

// ─── HubState ───────────────────────────────────────────────────────────────

class PYLABHUB_UTILS_EXPORT HubState
{
  public:
    HubState();
    ~HubState();
    HubState(const HubState &)            = delete;
    HubState &operator=(const HubState &) = delete;
    HubState(HubState &&)                 = delete;
    HubState &operator=(HubState &&)      = delete;

    // ── Read-only accessors (shared lock; copy out) ─────────────────────
    [[nodiscard]] HubStateSnapshot            snapshot() const;
    [[nodiscard]] std::optional<ChannelEntry> channel(const std::string &name) const;
    [[nodiscard]] std::optional<RoleEntry>    role(const std::string &uid) const;
    [[nodiscard]] std::optional<BandEntry>    band(const std::string &name) const;
    [[nodiscard]] std::optional<PeerEntry>    peer(const std::string &hub_uid) const;
    [[nodiscard]] std::optional<ShmBlockRef>  shm_block(const std::string &channel_name) const;
    [[nodiscard]] BrokerCounters              counters() const;

    // ── Event subscription ──────────────────────────────────────────────
    using ChannelOpenedHandler        = std::function<void(const ChannelEntry &)>;
    using ChannelStatusChangedHandler = std::function<void(const ChannelEntry &)>;
    using ChannelClosedHandler        = std::function<void(const std::string & /*name*/)>;
    using ConsumerAddedHandler        = std::function<void(const std::string & /*channel*/,
                                                           const ConsumerEntry &)>;
    using ConsumerRemovedHandler      = std::function<void(const std::string & /*channel*/,
                                                           const std::string & /*role_uid*/)>;
    using RoleRegisteredHandler       = std::function<void(const RoleEntry &)>;
    using RoleDisconnectedHandler     = std::function<void(const std::string & /*role_uid*/)>;
    using BandJoinedHandler           = std::function<void(const std::string & /*band*/,
                                                           const BandMember &)>;
    using BandLeftHandler             = std::function<void(const std::string & /*band*/,
                                                           const std::string & /*role_uid*/)>;
    using PeerConnectedHandler        = std::function<void(const PeerEntry &)>;
    using PeerDisconnectedHandler     = std::function<void(const std::string & /*hub_uid*/)>;

    HandlerId subscribe_channel_opened(ChannelOpenedHandler h);
    HandlerId subscribe_channel_status_changed(ChannelStatusChangedHandler h);
    HandlerId subscribe_channel_closed(ChannelClosedHandler h);
    HandlerId subscribe_consumer_added(ConsumerAddedHandler h);
    HandlerId subscribe_consumer_removed(ConsumerRemovedHandler h);
    HandlerId subscribe_role_registered(RoleRegisteredHandler h);
    HandlerId subscribe_role_disconnected(RoleDisconnectedHandler h);
    HandlerId subscribe_band_joined(BandJoinedHandler h);
    HandlerId subscribe_band_left(BandLeftHandler h);
    HandlerId subscribe_peer_connected(PeerConnectedHandler h);
    HandlerId subscribe_peer_disconnected(PeerDisconnectedHandler h);
    void      unsubscribe(HandlerId id) noexcept;

  private:
    friend class ::pylabhub::broker::BrokerService;
    friend class ::pylabhub::broker::BrokerServiceImpl;
    friend struct ::pylabhub::hub::test::HubStateTestAccess;

    // ── Private mutators (friend-only) ──────────────────────────────────
    // Pattern per mutator: acquire unique_lock on state → update map →
    // release state lock → snapshot handler list → invoke handlers with
    // both locks released.
    void _set_channel_opened(ChannelEntry entry);
    void _set_channel_status(const std::string &name, ChannelStatus s);
    void _set_channel_closed(const std::string &name);
    void _add_consumer(const std::string &channel, ConsumerEntry entry);
    void _remove_consumer(const std::string &channel, const std::string &role_uid);
    void _set_role_registered(RoleEntry entry);
    void _update_role_heartbeat(const std::string                     &uid,
                                std::chrono::steady_clock::time_point  when);
    void _update_role_metrics(const std::string                     &uid,
                              nlohmann::json                         metrics,
                              std::chrono::system_clock::time_point  when);
    void _set_role_disconnected(const std::string &uid);
    void _set_band_joined(const std::string &band, BandMember member);
    void _set_band_left(const std::string &band, const std::string &role_uid);
    void _set_peer_connected(PeerEntry entry);
    void _set_peer_disconnected(const std::string &hub_uid);
    /// Set the grace deadline on a channel that has transitioned to
    /// Closing.  No-op if the channel is unknown.  Caller is expected
    /// to also set status=Closing via `_set_channel_status`; this
    /// primitive only writes the deadline field so the broker's
    /// grace-expiry sweep observes a consistent value through HubState.
    void _set_channel_closing_deadline(const std::string                    &name,
                                        std::chrono::steady_clock::time_point deadline);

    /// Update the producer's `zmq_node_endpoint` for a channel
    /// (HEP-CORE-0021 ZMQ endpoint registry — ENDPOINT_UPDATE_REQ).
    /// No-op if the channel is unknown.  Endpoint validation is the
    /// caller's responsibility (broker handler runs validate_tcp_endpoint
    /// before calling this).
    void _set_channel_zmq_node_endpoint(const std::string &name,
                                         std::string        endpoint);

    void _set_shm_block(ShmBlockRef ref);
    void _bump_counter(const std::string &key, uint64_t n = 1);
    /// Bump `msg_type_errors[<msg_type>]` (HEP-CORE-0033 §9.4). Called
    /// by the broker dispatcher when a known-msg_type handler hit an
    /// exception or validation rejection.  Atomic with the
    /// `_on_message_processed` bump that always fires for the same
    /// message.
    void _bump_msg_type_error(const std::string &msg_type, uint64_t n = 1);

    // ── Capability-operation layer (HEP-0033 §G2) ──────────────────────
    //
    // Each `_on_*` represents one inbound wire message or sweep event
    // as a single hub-level operation; internally it composes the
    // primitive `_set_*` setters above.  Reshapes HubState's mutator
    // surface from "field setters" into "hub capabilities" so callers
    // don't have to remember which primitives belong together.
    //
    // Atomicity: each op takes the state lock per-primitive (not once
    // for the whole op).  `snapshot()` consumers always see consistent
    // state at the field level; single-entry lookups between primitives
    // of the same op may observe partial state.  This matches today's
    // broker, which touches ChannelRegistry, metrics_store_, and
    // counters under separate locks.  If stricter atomicity is needed
    // later, promote primitives to `_locked` variants and refactor ops
    // to acquire the writer lock once.
    //
    // role_tag derivation: today's wire protocol (REG_REQ /
    // CONSUMER_REG_REQ / BAND_JOIN_REQ) does not carry `role_tag`;
    // RoleEntry.role_tag is left empty when auto-derived from these
    // messages.  Admin / script paths may fill it in later.

    void _on_channel_registered(ChannelEntry entry);
    void _on_channel_closed(const std::string &name, ChannelCloseReason why);
    void _on_consumer_joined(const std::string &channel, ConsumerEntry consumer);
    void _on_consumer_left(const std::string &channel, const std::string &role_uid);
    void _on_heartbeat(const std::string                           &channel,
                       const std::string                           &role_uid,
                       std::chrono::steady_clock::time_point        when,
                       const std::optional<nlohmann::json>         &metrics);
    void _on_heartbeat_timeout(const std::string &channel, const std::string &role_uid);
    void _on_pending_timeout(const std::string &channel);
    /// Dedicated metrics-report wire message (HEP-0033 §9.1 "Metrics report
    /// tick"). `_on_heartbeat` handles the piggyback case; this op handles
    /// the time-only METRICS_REPORT_REQ path that fires even when the role
    /// has stopped its iteration-gated heartbeat cadence.
    void _on_metrics_reported(const std::string                    &channel,
                              const std::string                    &role_uid,
                              nlohmann::json                        metrics,
                              std::chrono::system_clock::time_point when);
    void _on_band_joined(const std::string &band, BandMember member);
    void _on_band_left(const std::string &band, const std::string &role_uid);
    void _on_peer_connected(PeerEntry peer);
    void _on_peer_disconnected(const std::string &hub_uid);
    void _on_message_processed(const std::string &msg_type,
                               std::size_t        bytes_in,
                               std::size_t        bytes_out);

    struct Impl;
#if defined(_MSC_VER)
#pragma warning(suppress : 4251)
#endif
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::hub
