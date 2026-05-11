#pragma once
/**
 * @file hub_state.hpp
 * @brief HubState — read-mostly aggregate of the hub's authoritative state.
 *
 * HEP-CORE-0033 §8 (data shape + accessors) + the "broker performs
 * state changes; HubState exposes only const snapshot accessors"
 * model HEP-0033 §4 / §8 / §9 establish.  HEP-CORE-0034 adds the
 * owner-keyed schema registry (`schemas` map).
 *
 * Status: G2.2 absorption complete; HEP-0034 Phase 2-3 wired through.
 * BrokerService calls `_on_*` capability ops directly from its REG_REQ
 * / CONSUMER_REG_REQ / DISC_REQ / heartbeat handlers; there are no
 * remaining broker-side maps that duplicate HubState.  Schema records
 * are populated by REG_REQ and cascade-evicted when the owning role
 * transitions to Disconnected (HEP-CORE-0034 §7.2).
 *
 * Ownership
 * ---------
 * HubHost owns the single HubState. BrokerService holds a `HubState&`
 * and is the only caller of the private `_set_*` and `_on_*` mutators
 * — enforced by `friend class broker::BrokerService`.  Scripts and
 * admin reach state via HubAPI / AdminService, which hold:
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
 */

#include "pylabhub_utils_export.h"
#include "utils/channel_pattern.hpp"
#include "utils/json_fwd.hpp"
#include "utils/schema_record.hpp"  // SchemaRecord, SchemaRegOutcome, CitationOutcome

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>  // std::pair
#include <vector>

namespace pylabhub::broker
{
class BrokerService;
class BrokerServiceImpl; ///< pImpl; grants the impl class friend access too.
}

namespace pylabhub::hub
{

// ─── Enums ──────────────────────────────────────────────────────────────────

enum class RoleState
{
    Connected,    ///< Heartbeats flowing within `effective_ready_timeout`.
    Pending,      ///< Heartbeats stalled but recoverable (the next matching
                  ///< heartbeat returns the presence to Connected).  Per
                  ///< HEP-CORE-0023 §2.1.
    Disconnected, ///< Terminal.  Presence row reaped.  For producer-presences,
                  ///< channel teardown fires atomically (HEP-CORE-0023 §2.1):
                  ///< broker emits CHANNEL_CLOSING_NOTIFY (best-effort) and
                  ///< removes the channel entry in the same handler.
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

PYLABHUB_UTILS_EXPORT const char *to_string(RoleState          s) noexcept;
PYLABHUB_UTILS_EXPORT const char *to_string(PeerState          s) noexcept;
PYLABHUB_UTILS_EXPORT const char *to_string(ChannelCloseReason r) noexcept;

/// Derived view of a channel's current observability, computed from the
/// producer-role's presence FSM (HEP-CORE-0023 §2.2 + §2.6).  Channel
/// state is NOT stored on `ChannelEntry`; this enum is the result of
/// `ChannelEntry::observe(producer_presence)`.
///
/// Mapping (producer-presence state -> channel observable):
///   - producer-presence absent / Disconnected → kAbsent
///   - Connected + !first_heartbeat_seen       → kRegistering
///   - Pending                                 → kStalled
///   - Connected +  first_heartbeat_seen       → kLive
enum class ChannelObservable
{
    kAbsent,       ///< No producer registered, or producer-presence Disconnected.
    kRegistering,  ///< Producer registered, no heartbeat received yet.
    kStalled,      ///< Producer-presence Pending (heartbeats stalled but recoverable).
    kLive,         ///< Producer-presence Connected with fresh heartbeats.
};

PYLABHUB_UTILS_EXPORT const char *to_string(ChannelObservable o) noexcept;

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

/// Producer attached to a channel.  Parallel shape to ConsumerEntry —
/// channels admit 1..N producers per HEP-CORE-0023 §2.1.1 (ZMQ
/// Fan-In; SHM stays single-producer by physical constraint).
///
/// Each producer carries its own inbox (HEP-CORE-0027): the endpoint
/// other roles send to in order to reach THIS producer.  Inbox info is
/// per-producer, not channel-wide, so that multi-producer channels
/// preserve each producer's distinct inbox routing.
struct ProducerEntry
{
    uint64_t    producer_pid{0};
    std::string producer_hostname;
    std::string zmq_identity; ///< ROUTER identity for direct notifications.

    std::string role_name;
    std::string role_uid;

    // Inbox (HEP-CORE-0027) — symmetric with ConsumerEntry's inbox_*.
    std::string inbox_endpoint;
    std::string inbox_schema_json;
    std::string inbox_packing;  ///< "aligned" | "packed"
    std::string inbox_checksum; ///< "enforced" | "manual" | "none"

    // ── Wave M2.5 step 2a — per-producer fields ────────────────────────
    //
    // Migrated from `ChannelEntry` because they are producer
    // attributes, not channel-wide invariants.  Each Fan-In producer
    // can carry its own endpoint + metadata blob (see
    // docs/tech_draft/controlled_access_api_design.md §3.2 + §6.1).
    //
    // STEP 2a ADDITIVE: these fields are added here; the matching
    // fields on `ChannelEntry` remain until step 2b/3 migrates the
    // callers (REG_REQ handler, DISC_REQ_ACK, ENDPOINT_UPDATE_REQ).
    // After step 2b/3 those channel-scope copies are deleted.

    /// HEP-CORE-0021 §16 — this producer's data-plane bound endpoint.
    /// For SHM channels this is empty; for ZMQ channels it is the
    /// producer's PUB socket address (each producer in a Fan-In
    /// channel has its own).
    std::string zmq_node_endpoint;

    /// Producer-supplied free-form context blob (HEP-CORE-0007 §12.4).
    /// `null` if no metadata.  Channel-level DISC_REQ_ACK aggregates
    /// all producers' blobs into a tree keyed by `role_uid` via
    /// `ChannelEntry::aggregate_metadata_tree()`.
    nlohmann::json metadata;

    std::chrono::system_clock::time_point connected_at{
        std::chrono::system_clock::now()};
};

// ─── Controlled-access API result types (Wave M2.5) ───────────────────────
//
// Producer + consumer admission is strictly additive — no restart-replace
// path exists.  Any incoming admission carrying a `role_uid` that already
// exists in the channel's `producers[]` / `consumers[]` is refused with
// `RejectedUidConflict`, regardless of whether the existing entry is
// active or stale-residue.  The contract is documented in
// `docs/tech_draft/controlled_access_api_design.md` §6.2; the rationale
// is that proper uid construction (`tag.name.unique` per HEP-CORE-0033
// §G2.2.0b) makes a same-uid collision effectively impossible, so any
// collision indicates either bookkeeping residue (a hub-side bug — to
// be fixed in M3 cleanup) or a remote-side breach attempt.

enum class AddProducerResult
{
    Created,                 ///< Appended; out-pointer (if requested) points
                             ///< to the new ProducerEntry in `producers[]`.
    RejectedUidConflict,     ///< `find_producer(role_uid)` returned non-null.
                             ///< Channel state unchanged.  Broker surfaces
                             ///< `UID_CONFLICT` on the wire.
    RejectedShmCardinality,  ///< `data_transport == "shm"` and
                             ///< `producers.size() >= 1`.  Channel state
                             ///< unchanged.  Broker surfaces
                             ///< `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM`.
};

enum class AddConsumerResult
{
    Created,                 ///< Appended; out-pointer (if requested) points
                             ///< to the new ConsumerEntry in `consumers[]`.
    RejectedUidConflict,     ///< Existing ConsumerEntry with this role_uid.
};

struct RemoveProducerResult
{
    bool removed;             ///< True iff a producer with the given uid
                              ///< was present and has been erased.
    bool channel_now_empty;   ///< True iff `producers.empty()` after the
                              ///< removal.  Broker uses this to decide
                              ///< whether to trigger `_on_channel_closed`.
};

/// Result of a `ChannelEntry::set_invariant_*` call.  Channel-wide
/// invariants are values that all producers must agree on (schema,
/// transport choice, pattern).  First setter creates; subsequent
/// setters either match byte-for-byte (idempotent) or are rejected.
enum class InvariantSetResult
{
    Created,                 ///< Field(s) had no prior value; set succeeded.
    IdempotentEqual,         ///< Existing value equals the requested value;
                             ///< no change made.
    RejectedMismatch,        ///< Existing value differs.  Channel state
                             ///< unchanged.  Broker surfaces the appropriate
                             ///< Cat-1 error (e.g., `SCHEMA_MISMATCH`).
};

/// Channel registered with the broker.
struct ChannelEntry
{
    std::string name;          ///< Key in HubState::channels.
    std::string shm_name;
    std::string schema_hash;   ///< Hex (64 chars).  Channel-wide
                               ///< invariant — all producers MUST
                               ///< agree (HEP-CORE-0023 §2.1.1).
    uint32_t    schema_version{0};
    std::string schema_id;     ///< Named-schema id; empty = anonymous.
    std::string schema_blds;

    /// HEP-CORE-0034 §11 — foreign key into `HubState.schemas`.  When
    /// non-empty, identifies the schema record this channel is contracted
    /// against.  Values: `"hub"` if the channel adopts a hub-global, or
    /// a producer's role uid if that producer registered its own private
    /// record via path-B.  Anonymous (legacy) channels leave both
    /// `schema_owner` and `schema_id` empty.
    std::string schema_owner;

    nlohmann::json             metadata;
    std::vector<ProducerEntry> producers; ///< 1..N producers (HEP-0023 §2.1.1).
    std::vector<ConsumerEntry> consumers;

    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    zmq_ctrl_endpoint;
    std::string    zmq_data_endpoint;
    std::string    zmq_pubkey;
    std::string    data_transport{"shm"};
    std::string    zmq_node_endpoint;

    // Inbox info is per-producer (HEP-CORE-0023 §2.1.1 + HEP-CORE-0027);
    // see ProducerEntry.inbox_*.  Removed from ChannelEntry 2026-05-10
    // as part of Wave M2 MP2 because multi-producer channels need
    // distinct inbox routing per producer — the prior channel-wide
    // scalar fields would have silently overwritten on second REG_REQ.

    std::chrono::system_clock::time_point created_at{
        std::chrono::system_clock::now()};

    // ── Producer-list helpers ───────────────────────────────────────
    //
    // Channel existence is "any live producer-presence" (HEP-CORE-0023
    // §2.1.1).  Most caller intents fall into three shapes:
    //   - "the single producer" — common for SHM channels; use
    //     `first_producer()` and accept it as the only one.
    //   - "this specific producer" — REG_REQ admission, DEREG_REQ,
    //     CHANNEL_ERROR_NOTIFY target; use `find_producer(role_uid)`.
    //   - "every producer" — fan-out paths (CHANNEL_CLOSING_NOTIFY,
    //     CHANNEL_ERROR_NOTIFY broadcast); iterate `producers`
    //     directly.

    /// Returns the first registered producer, or nullptr if none.
    /// Convenience for callers that operate on the single-producer
    /// invariant (e.g. SHM channels).
    const ProducerEntry *first_producer() const noexcept
    {
        return producers.empty() ? nullptr : &producers.front();
    }
    ProducerEntry *first_producer() noexcept
    {
        return producers.empty() ? nullptr : &producers.front();
    }

    /// Returns the producer whose role_uid matches, or nullptr.
    const ProducerEntry *find_producer(const std::string &role_uid) const noexcept
    {
        for (const auto &p : producers)
            if (p.role_uid == role_uid) return &p;
        return nullptr;
    }
    ProducerEntry *find_producer(const std::string &role_uid) noexcept
    {
        for (auto &p : producers)
            if (p.role_uid == role_uid) return &p;
        return nullptr;
    }

    /// Derived: compute this channel's current observability from a
    /// producer-presence pointer (HEP-CORE-0023 §2.2 + §2.6).  Pure
    /// function — no HubState lookup, no locking.  The caller resolves
    /// the producer-presence pointer (via
    /// `RoleEntry::find_presence(name, "producer")`) and passes it in;
    /// `nullptr` means "no producer-presence registered" → kAbsent.
    ///
    /// Multi-producer note: this overload still takes a single
    /// presence — the channel's observable is `kLive` iff ANY of its
    /// producer-presences is `Connected` with `first_heartbeat_seen`.
    /// `observe_channel(c, snapshot)` (free function below) does the
    /// "best of all producers" scan.  This pointer-form variant is
    /// retained for callers that already know which presence they
    /// care about (e.g., DISC_REQ probing a specific producer).
    inline ChannelObservable observe(const struct RolePresence *producer) const noexcept;

    // ── Wave M2.5 controlled-access API ─────────────────────────────
    //
    // Additive: methods that callers should migrate to.  Fields above
    // remain public during this wave; once all callers go through
    // these methods, the state-bearing fields move private (M2.5
    // step 7).  See `docs/tech_draft/controlled_access_api_design.md`.

    [[nodiscard]] std::size_t producer_count() const noexcept { return producers.size(); }
    [[nodiscard]] std::size_t consumer_count() const noexcept { return consumers.size(); }
    [[nodiscard]] bool        is_shm() const noexcept { return data_transport == "shm"; }

    /// Find the ConsumerEntry for `role_uid`, or nullptr.  Mirror of
    /// `find_producer` for symmetry — used by the new `add_consumer`
    /// reject path and by DEREG-handler call sites.
    const ConsumerEntry *find_consumer(const std::string &role_uid) const noexcept
    {
        for (const auto &c : consumers)
            if (c.role_uid == role_uid) return &c;
        return nullptr;
    }
    ConsumerEntry *find_consumer(const std::string &role_uid) noexcept
    {
        for (auto &c : consumers)
            if (c.role_uid == role_uid) return &c;
        return nullptr;
    }

    /// Strict additive producer admission (HEP-CORE-0023 §2.1.1 + M2.5).
    /// See `AddProducerResult` doc above for the contract.  When the
    /// reply is `Created`, the new entry is at `producers.back()` and
    /// (if `out` is non-null) `*out` points to it.
    AddProducerResult add_producer(ProducerEntry p,
                                    ProducerEntry **out = nullptr)
    {
        if (find_producer(p.role_uid) != nullptr)
            return AddProducerResult::RejectedUidConflict;
        if (is_shm() && !producers.empty())
            return AddProducerResult::RejectedShmCardinality;
        producers.push_back(std::move(p));
        if (out != nullptr) *out = &producers.back();
        return AddProducerResult::Created;
    }

    /// Remove the producer entry whose role_uid matches.  Reports
    /// whether the channel is now empty (no remaining producers) so
    /// callers can trigger atomic teardown per HEP-CORE-0023 §2.1.1.
    RemoveProducerResult remove_producer(std::string_view role_uid) noexcept
    {
        for (auto it = producers.begin(); it != producers.end(); ++it)
        {
            if (it->role_uid == role_uid)
            {
                producers.erase(it);
                return {true, producers.empty()};
            }
        }
        return {false, producers.empty()};
    }

    /// Strict additive consumer admission (symmetric to add_producer).
    AddConsumerResult add_consumer(ConsumerEntry c,
                                    ConsumerEntry **out = nullptr)
    {
        if (find_consumer(c.role_uid) != nullptr)
            return AddConsumerResult::RejectedUidConflict;
        consumers.push_back(std::move(c));
        if (out != nullptr) *out = &consumers.back();
        return AddConsumerResult::Created;
    }

    /// Remove the consumer entry whose role_uid matches.  Returns
    /// true iff an entry was erased.
    bool remove_consumer(std::string_view role_uid) noexcept
    {
        for (auto it = consumers.begin(); it != consumers.end(); ++it)
        {
            if (it->role_uid == role_uid)
            {
                consumers.erase(it);
                return true;
            }
        }
        return false;
    }

    /// Update the per-producer inbox metadata (HEP-CORE-0027 §3).
    /// Returns true iff the producer was found.  Inbox is per-producer:
    /// each producer on a Fan-In channel keeps its own inbox routing.
    bool set_producer_inbox(std::string_view role_uid,
                             std::string endpoint, std::string schema_json,
                             std::string packing,  std::string checksum) noexcept
    {
        for (auto &p : producers)
        {
            if (p.role_uid == role_uid)
            {
                p.inbox_endpoint    = std::move(endpoint);
                p.inbox_schema_json = std::move(schema_json);
                p.inbox_packing     = std::move(packing);
                p.inbox_checksum    = std::move(checksum);
                return true;
            }
        }
        return false;
    }

    /// Symmetric per-consumer inbox setter.
    bool set_consumer_inbox(std::string_view role_uid,
                             std::string endpoint, std::string schema_json,
                             std::string packing,  std::string checksum) noexcept
    {
        for (auto &c : consumers)
        {
            if (c.role_uid == role_uid)
            {
                c.inbox_endpoint    = std::move(endpoint);
                c.inbox_schema_json = std::move(schema_json);
                c.inbox_packing     = std::move(packing);
                c.inbox_checksum    = std::move(checksum);
                return true;
            }
        }
        return false;
    }

    /// Set the per-producer data-plane ZMQ endpoint (HEP-CORE-0021).
    /// Returns true iff the producer was found.  Distinct from the
    /// channel-scope `ChannelEntry::zmq_node_endpoint` which is
    /// retained during step 2a for callers not yet migrated.
    bool set_producer_zmq_node_endpoint(std::string_view role_uid,
                                          std::string endpoint) noexcept
    {
        for (auto &p : producers)
        {
            if (p.role_uid == role_uid)
            {
                p.zmq_node_endpoint = std::move(endpoint);
                return true;
            }
        }
        return false;
    }

    /// Look up a producer's data-plane ZMQ endpoint; nullopt if the
    /// uid is not registered on this channel.
    std::optional<std::string>
    producer_zmq_node_endpoint(std::string_view role_uid) const
    {
        for (const auto &p : producers)
            if (p.role_uid == role_uid) return p.zmq_node_endpoint;
        return std::nullopt;
    }

    /// Set the per-producer metadata blob (HEP-CORE-0007 §12.4).
    /// Returns true iff the producer was found.  Producers may
    /// publish orthogonal blobs; channel-level read paths aggregate
    /// them via `aggregate_metadata_tree()`.
    bool set_producer_metadata(std::string_view role_uid,
                                nlohmann::json blob) noexcept
    {
        for (auto &p : producers)
        {
            if (p.role_uid == role_uid)
            {
                p.metadata = std::move(blob);
                return true;
            }
        }
        return false;
    }

    /// Look up a single producer's metadata blob; nullptr if absent.
    const nlohmann::json *producer_metadata(std::string_view role_uid) const noexcept
    {
        for (const auto &p : producers)
            if (p.role_uid == role_uid) return &p.metadata;
        return nullptr;
    }

    /// Channel-level metadata read: aggregate all producers' metadata
    /// blobs into a tree keyed by `role_uid` (HEP-CORE-0007 §12.4 +
    /// docs/tech_draft/controlled_access_api_design.md §6.1).
    ///
    /// Wire shape (returned object):
    /// ```
    /// {
    ///   "<producer_role_uid_1>": { ...blob1... },
    ///   "<producer_role_uid_2>": { ...blob2... }
    /// }
    /// ```
    /// A producer whose metadata is `null` is omitted from the tree —
    /// the result is `{}`, never `null`, so consumers can rely on
    /// the field being an object.
    nlohmann::json aggregate_metadata_tree() const
    {
        nlohmann::json out = nlohmann::json::object();
        for (const auto &p : producers)
        {
            if (!p.metadata.is_null()) out[p.role_uid] = p.metadata;
        }
        return out;
    }
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

/// One per-`(channel, role_type)` row under a RoleEntry.  A role with N
/// presences runs N independent FSMs and writes N distinct rows in the
/// per-presence metrics store.  Per HEP-CORE-0023 §2.1 + §2.6.
///
/// A processor with `(uid, "consumer")` on `in_channel` and
/// `(uid, "producer")` on `out_channel` carries two `RolePresence`
/// entries; each refreshes only on its own matching `HEARTBEAT_REQ`.
///
/// Source of truth for the per-presence FSM (state, last_heartbeat,
/// metrics).  Channel state is derived from the producer-presence —
/// see `ChannelEntry::observe()`.
struct RolePresence
{
    std::string channel;    ///< Channel this presence is registered on.
    std::string role_type;  ///< "producer" | "consumer".

    RoleState   state{RoleState::Connected};
    /// Set true once at least one HEARTBEAT_REQ matching this presence's
    /// `(uid, channel, role_type)` has been received.  False between
    /// REG_REQ acceptance and first heartbeat.  DISC_REQ uses this to
    /// distinguish "registered but no heartbeat yet" (DISC_PENDING with
    /// reason="awaiting_first_heartbeat") from "live" (DISC_ACK).
    bool        first_heartbeat_seen{false};

    std::chrono::steady_clock::time_point last_heartbeat{
        std::chrono::steady_clock::now()};
    /// Wall-clock instant of the last FSM transition (Connected ↔ Pending,
    /// or initial Connected on REG).  Used by sweep loops and diagnostics.
    std::chrono::steady_clock::time_point state_since{
        std::chrono::steady_clock::now()};

    nlohmann::json                        latest_metrics;
    std::chrono::system_clock::time_point metrics_collected_at{};
};

/// Registered role (plh_role process). Independent of channel membership.
struct RoleEntry
{
    std::string uid;       ///< Key in HubState::roles.
    std::string name;
    std::string role_tag;  ///< "prod" / "cons" / "proc" or a custom tag.
    std::vector<std::string> channels;

    std::chrono::system_clock::time_point first_seen{
        std::chrono::system_clock::now()};

    std::string pubkey_z85; ///< Role's CurveZMQ public key (Z85, 40 chars).

    /// Per-presence rows.  One row per `(channel, role_type)` this role
    /// uid is registered on.  Created eagerly at REG_REQ / CONSUMER_REG_REQ
    /// time per HEP-CORE-0023 §2.6 — never lazy.  All FSM state, heartbeats,
    /// and metrics live here; nothing is duplicated at the role level.
    std::vector<RolePresence> presences;

    /// 🚧 PATCH (2026-05-10) — stopgap; full fix pending in Wave M2 MP3.
    ///
    /// Event-emit memoization: has `role_disconnected` already fired for
    /// this role since it was registered (or last revived)?  Set by the
    /// disconnect cascade after a successful emit; cleared whenever a
    /// fresh presence is added / an existing presence transitions back
    /// to Connected.  Idempotency contract for `_set_role_disconnected`
    /// and the unified cascade (HEP-CORE-0023 §2.1).  NOT a duplicate
    /// FSM state — `any_presence_alive()` remains the source of truth
    /// for whether the role is currently live.
    ///
    /// **Why this is a patch:** M1.2 deleted `RoleEntry.state` (the
    /// pre-sweep idempotency gate); the per-presence FSM is the source
    /// of truth for liveness, but "has the role_disconnected event
    /// already been emitted?" is a separate concern that the deleted
    /// `state` field also tracked.  M2 MP3 (`_dispatch_role_disconnected_if_dead`
    /// helper called from every cascade path) will own this exclusively;
    /// the right home for the memoization (this flag vs. an
    /// `HubState::Impl`-level set vs. caller-supplied "was-alive" signal)
    /// is locked in MP3.  Until MP3 lands, this flag is the temporary
    /// home — see `docs/TODO_MASTER.md` "Wave M2".
    bool disconnected_fired{false};

    /// Find the presence row matching `(channel, role_type)`.  Returns
    /// nullptr if not found.  Linear scan — `presences` is small
    /// (typically 1, occasionally 2 for processors).  Inline so callers
    /// in tests / headers don't need a library symbol.
    const RolePresence *find_presence(const std::string &channel,
                                       const std::string &role_type) const noexcept
    {
        for (const auto &p : presences)
            if (p.channel == channel && p.role_type == role_type) return &p;
        return nullptr;
    }
    RolePresence *find_presence(const std::string &channel,
                                 const std::string &role_type) noexcept
    {
        for (auto &p : presences)
            if (p.channel == channel && p.role_type == role_type) return &p;
        return nullptr;
    }

    /// Per-uid liveness: is ANY presence still alive (not Disconnected)?
    /// Per HEP-CORE-0023 §2.6 line 504-507 — derived, not stored.
    /// Used by admin queries that ask "is this role still around?" and by
    /// the role-reaper to decide whether to delete the whole `RoleEntry`
    /// when its last presence transitions Disconnected.
    [[nodiscard]] bool any_presence_alive() const noexcept
    {
        for (const auto &p : presences)
            if (p.state != RoleState::Disconnected) return true;
        return false;
    }
};

// ChannelEntry::observe — defined inline after RolePresence is complete.
inline ChannelObservable
ChannelEntry::observe(const RolePresence *producer) const noexcept
{
    if (producer == nullptr)
        return ChannelObservable::kAbsent;
    if (producer->state == RoleState::Disconnected)
        return ChannelObservable::kAbsent;
    if (!producer->first_heartbeat_seen)
        return ChannelObservable::kRegistering;
    if (producer->state == RoleState::Pending)
        return ChannelObservable::kStalled;
    return ChannelObservable::kLive;  // Connected + first_heartbeat_seen
}

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

    // ── Schema-registry counters (HEP-CORE-0034 §11.3) ──────────────────
    /// Bumped each time `_on_schema_registered` returns Created.
    uint64_t schema_registered_total{0};
    /// Bumped per record removed by `_on_schemas_evicted_for_owner`.
    uint64_t schema_evicted_total{0};
    /// Bumped each time `_validate_schema_citation` returns non-ok.
    uint64_t schema_citation_rejected_total{0};
};

/// Key for the schemas map: `(owner_uid, schema_id)`.
/// HEP-CORE-0034 §4.2 — namespace-by-owner; two roles may both register
/// `frame` and they live under separate keys.
using SchemaKey = std::pair<std::string, std::string>;

/// Point-in-time aggregate returned by HubState::snapshot().
struct HubStateSnapshot
{
    std::unordered_map<std::string, ChannelEntry> channels;
    std::unordered_map<std::string, RoleEntry>    roles;
    std::unordered_map<std::string, BandEntry>    bands;
    std::unordered_map<std::string, PeerEntry>    peers;
    std::unordered_map<std::string, ShmBlockRef>  shm_blocks;
    /// HEP-CORE-0034 §11.1 — owner-keyed schema records.
    /// `std::map` (not unordered) so iteration is deterministic for tests
    /// and admin diagnostics; the table is small (one record per role
    /// + hub-globals) so the log-N cost is irrelevant.
    std::map<SchemaKey, schema::SchemaRecord>     schemas;
    BrokerCounters                                counters;
    std::chrono::system_clock::time_point         captured_at{
        std::chrono::system_clock::now()};
};

/// Resolve a channel's current observable from a hub snapshot.  Scans
/// every registered producer-role's presence on this channel and
/// returns the "best" outcome: kLive if any producer is Connected
/// with first_heartbeat_seen; kRegistering if at least one producer
/// is Connected awaiting first heartbeat; kStalled if every producer
/// is Pending; kAbsent if no producer-presence is registered or all
/// are Disconnected.  Per HEP-CORE-0023 §2.1.1 (multi-producer
/// channels: channel is alive iff any producer-presence is alive).
///
/// Used by every JSON serializer that surfaces channel state on the
/// wire (HEP-CORE-0023 §2.2 — `observable` is the protocol-defined
/// field).
///
/// Templated on the roles-map type so the SAME logic serves both the
/// snapshot path (`HubStateSnapshot::roles`) and any internal caller
/// that needs to compute the observable while holding a writer lock
/// over the live HubState (e.g., `_on_heartbeat` after a presence
/// transition).  Both paths use `std::unordered_map<std::string,
/// RoleEntry>` — the template avoids duplicating the scan logic.
template <typename RolesMap>
inline ChannelObservable
compute_channel_observable(const ChannelEntry &ch, const RolesMap &roles) noexcept
{
    if (ch.producers.empty()) return ChannelObservable::kAbsent;

    bool any_live        = false;
    bool any_registering = false;
    bool any_stalled     = false;
    for (const auto &prod : ch.producers)
    {
        auto rit = roles.find(prod.role_uid);
        if (rit == roles.end()) continue;
        const auto *p = rit->second.find_presence(ch.name, "producer");
        if (p == nullptr) continue;
        switch (ch.observe(p))
        {
        case ChannelObservable::kLive:        any_live = true;        break;
        case ChannelObservable::kRegistering: any_registering = true; break;
        case ChannelObservable::kStalled:     any_stalled = true;     break;
        case ChannelObservable::kAbsent:                              break;
        }
    }
    if (any_live)        return ChannelObservable::kLive;
    if (any_registering) return ChannelObservable::kRegistering;
    if (any_stalled)     return ChannelObservable::kStalled;
    return ChannelObservable::kAbsent;
}

/// Snapshot-side wrapper for the common caller pattern: pass in a
/// `HubStateSnapshot` and get the channel's observable.  Delegates to
/// the templated `compute_channel_observable`; kept for source-level
/// compatibility with existing call sites.
inline ChannelObservable
observe_channel(const ChannelEntry &ch, const HubStateSnapshot &snap) noexcept
{
    return compute_channel_observable(ch, snap.roles);
}

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

    /// Look up one schema record by `(owner_uid, schema_id)`.  Returns
    /// nullopt if the record does not exist.  Per HEP-CORE-0034 §11 the
    /// hub is the authoritative source for schema records — citers should
    /// ground their assumptions in this lookup, not in their local
    /// `<role_dir>/schemas/` cache.
    [[nodiscard]] std::optional<schema::SchemaRecord>
    schema(const std::string &owner_uid, const std::string &schema_id) const;

    /// Number of schema records currently stored.  Cheap read; useful for
    /// tests and admin diagnostics.
    [[nodiscard]] std::size_t schema_count() const;

    // ── Event subscription ──────────────────────────────────────────────
    using ChannelOpenedHandler        = std::function<void(const ChannelEntry &)>;
    using ChannelStatusChangedHandler =
        std::function<void(const ChannelEntry &, ChannelObservable)>;
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

    // Subscription / dispatch is logically `const` on the observable
    // HubState data: the `handlers_mu` mutex is `mutable` (declared in
    // the impl), and the registries it guards are independent of `mu`
    // which guards the channel/role/peer/band data.  Subscribers may
    // therefore register through a `const HubState&` — the same handle
    // returned by `HubHost::state()`.  See file-level "Thread model"
    // comment for the orthogonality between `mu` (data lock) and
    // `handlers_mu` (subscriber-registry lock).
    HandlerId subscribe_channel_opened(ChannelOpenedHandler h) const;
    HandlerId subscribe_channel_status_changed(ChannelStatusChangedHandler h) const;
    HandlerId subscribe_channel_closed(ChannelClosedHandler h) const;
    HandlerId subscribe_consumer_added(ConsumerAddedHandler h) const;
    HandlerId subscribe_consumer_removed(ConsumerRemovedHandler h) const;
    HandlerId subscribe_role_registered(RoleRegisteredHandler h) const;
    HandlerId subscribe_role_disconnected(RoleDisconnectedHandler h) const;
    HandlerId subscribe_band_joined(BandJoinedHandler h) const;
    HandlerId subscribe_band_left(BandLeftHandler h) const;
    HandlerId subscribe_peer_connected(PeerConnectedHandler h) const;
    HandlerId subscribe_peer_disconnected(PeerDisconnectedHandler h) const;
    void      unsubscribe(HandlerId id) const noexcept;

  private:
    friend class ::pylabhub::broker::BrokerService;
    friend class ::pylabhub::broker::BrokerServiceImpl;
    friend struct ::pylabhub::hub::test::HubStateTestAccess;

    // ── Private mutators (friend-only) ──────────────────────────────────
    // Pattern per mutator: acquire unique_lock on state → update map →
    // release state lock → snapshot handler list → invoke handlers with
    // both locks released.
    void _set_channel_opened(ChannelEntry entry);
    void _set_channel_closed(const std::string &name);
    void _add_consumer(const std::string &channel, ConsumerEntry entry);
    void _remove_consumer(const std::string &channel, const std::string &role_uid);
    void _set_role_registered(RoleEntry entry);
    void _set_role_disconnected(const std::string &uid);
    void _set_band_joined(const std::string &band, BandMember member);
    void _set_band_left(const std::string &band, const std::string &role_uid);
    void _set_peer_connected(PeerEntry entry);
    void _set_peer_disconnected(const std::string &hub_uid);

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
    // of the same op may observe partial state.  This matches the
    // broker's pattern of touching state and counters under separate
    // locks.  If stricter atomicity is needed later, promote
    // primitives to `_locked` variants and refactor ops to acquire
    // the writer lock once.
    //
    // role_tag derivation: today's wire protocol (REG_REQ /
    // CONSUMER_REG_REQ / BAND_JOIN_REQ) does not carry `role_tag`;
    // RoleEntry.role_tag is left empty when auto-derived from these
    // messages.  Admin / script paths may fill it in later.

    void _on_channel_registered(ChannelEntry entry);
    void _on_channel_closed(const std::string &name, ChannelCloseReason why);
    void _on_consumer_joined(const std::string &channel, ConsumerEntry consumer);
    void _on_consumer_left(const std::string &channel, const std::string &role_uid);
    /// Refresh the presence row matching `(channel, role_uid, role_type)`.
    /// Per HEP-CORE-0019 §2.3 + HEP-CORE-0023 §2.5.2: each heartbeat refreshes
    /// ONLY its own `(uid, role_type)` presence row; no heartbeat ever touches
    /// another presence's bookkeeping.  Presence rows must already exist
    /// (created at REG_REQ / CONSUMER_REG_REQ time per §2.6); a heartbeat for
    /// an unknown presence is a no-op.
    void _on_heartbeat(const std::string                           &channel,
                       const std::string                           &role_uid,
                       const std::string                           &role_type,
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

    // ── Schema-registry capability ops (HEP-CORE-0034 §11) ──────────────
    //
    // These are pure HubState mutations — Phase 2 surface only.  The
    // broker dispatcher will call them in Phase 3 from REG_REQ /
    // CONSUMER_REG_REQ / PROC_REG_REQ handlers.  Until then they exist
    // for L2 unit tests via HubStateTestAccess.

    /**
     * @brief Register or accept a schema record.
     *
     * The record's `registered_at` field is overwritten with the current
     * time before insertion (callers should leave it default-initialised).
     *
     * Conflict policy is namespace-by-owner (HEP-CORE-0034 §8): two
     * different `owner_uid` values for the same `schema_id` are independent
     * records, never collide.  Same-owner re-registration is idempotent if
     * the hash AND packing match exactly; rejected with `kHashMismatchSelf`
     * otherwise.
     *
     * Phase 2 enforces only the local invariants that HubState can verify:
     *   - `kForbiddenOwner` is reserved for Phase 3 broker validation
     *     (e.g. "this REG_REQ comes from role X but cites owner=hub
     *     without authorization") — Phase 2 returns `kForbiddenOwner` only
     *     if the record's `owner_uid` is empty.
     *
     * @return Created on insert, Idempotent on no-op success, or one of
     *         the rejection codes.  Counter `schema_registered_total` is
     *         bumped only on `kCreated`.
     */
    schema::SchemaRegOutcome _on_schema_registered(schema::SchemaRecord rec);

    /**
     * @brief Cascade-evict every schema record owned by `owner_uid`.
     *
     * Called from `_set_role_disconnected` (HEP-CORE-0034 §7.2) when a
     * role's process exits.  No-op if the role owns zero records or is
     * unknown.  Counter `schema_evicted_total` is bumped per record
     * removed.
     *
     * Caller invariant: this should be the only public path that removes
     * private schema records — hub-globals (owner=`"hub"`) are never
     * evicted by this op.
     *
     * @return Number of records actually removed.
     */
    std::size_t _on_schemas_evicted_for_owner(const std::string &owner_uid);

    /**
     * @brief Validate a citation for a connecting role / channel.
     *
     * Implements the HEP-CORE-0034 §9.1 invariant:
     *   - Cited owner must be either `"hub"` or `channel_producer_uid`.
     *   - Cited record must exist.
     *   - Cited record's hash AND packing must equal the citer's expected
     *     fingerprint.
     *
     * Cross-citation (owner is a third role) is rejected even on hash
     * match — see HEP-CORE-0034 §9.3 for rationale.  On any non-ok
     * outcome, counter `schema_citation_rejected_total` is bumped.
     *
     * @param citer_uid              Role uid making the citation (for diagnostics; not used for routing in Phase 2).
     * @param channel_producer_uid   Producer uid that owns the channel the citer is connecting to.
     * @param cited_owner            `"hub"` or producer uid as cited by the citer.
     * @param cited_id               Schema id under cited_owner's namespace.
     * @param expected_hash          Citer's expected fingerprint bytes (32).
     * @param expected_packing       Citer's expected packing string.
     */
    schema::CitationOutcome _validate_schema_citation(
        const std::string &citer_uid,
        const std::string &channel_producer_uid,
        const std::string &cited_owner,
        const std::string &cited_id,
        const std::array<uint8_t, 32> &expected_hash,
        const std::string &expected_packing);

    struct Impl;
#if defined(_MSC_VER)
#pragma warning(suppress : 4251)
#endif
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::hub
