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
#include <unordered_set>
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

    /// Consumer's CURVE pubkey (Z85, 40 chars).  Mirrors the producer-side
    /// `ProducerEntry::zmq_pubkey` field — populated from CONSUMER_REG_REQ
    /// wire body at REG accept (broker_proto 5→6 / PeerAdmission D3,
    /// HEP-CORE-0036 §6.5 amended 2026-06-04).  Used by the broker to
    /// populate `ChannelAccessEntry::authorized_consumer_pubkeys` via
    /// `_on_consumer_authorized` and to revoke via `_on_consumer_revoked`
    /// at DEREG / heartbeat timeout.  Empty for legacy consumers that
    /// don't carry the field; allowlist stays empty for them, which is
    /// the legal deny-all state per HEP-CORE-0035 §4.8.4.
    /// "No-self-claims" hardening (User-Id extraction) is a separate
    /// Layer-2 audit — see HEP-CORE-0036 §4.1 line 425.
    std::string zmq_pubkey;

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

    // ── Per-producer fields (Wave M2.5) ──────────────────────────────
    //
    // These fields live on `ProducerEntry` (not channel-scope) because
    // they are producer attributes — each Fan-In producer carries its
    // own endpoint, CURVE pubkey, and metadata blob.  See
    // docs/tech_draft/controlled_access_api_design.md §3.2 + §6.1 for
    // the design rationale and HEP-CORE-0021 §16.3 + §5.2 for the
    // wire-protocol references.
    //
    // Channel-level read paths aggregate when needed: metadata uses
    // `ChannelEntry::aggregate_metadata_tree()` (tree keyed by uid);
    // endpoints + pubkeys can be enumerated via `producers()` /
    // `find_producer(uid)`.  The legacy first-producer transitional
    // shape on DISC_REQ_ACK is documented in §7.5.3.

    /// HEP-CORE-0021 §16 — this producer's data-plane bound endpoint.
    /// For SHM channels this is empty; for ZMQ channels it is the
    /// producer's PUB socket address (each producer in a Fan-In
    /// channel has its own).
    std::string zmq_node_endpoint;

    /// HEP-CORE-0021 §5.2 + HEP-CORE-0002 — this producer's CURVE
    /// public key (Z85, 40 chars).  Used to authenticate the
    /// producer's ZMQ ctrl socket from the consumer side via
    /// `CONSUMER_REG_ACK.producer_zmq_pubkey`.  Per-producer by
    /// design (each producer publishes its own keypair).  Wave M2.5
    /// migration: the wire field name on REG_REQ is still
    /// `zmq_pubkey`; step 3 routes that into this field instead of
    /// the deprecated channel-scope `ChannelEntry.zmq_pubkey`.
    std::string zmq_pubkey;

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

/// Aggregate of channel-wide schema invariants supplied by REG_REQ.
/// Used by `HubState::_on_producer_added` to compare-and-set against
/// the existing channel record (HEP-CORE-0023 §2.1.1 + HEP-CORE-0034).
/// First admission on a fresh channel sets these; subsequent admissions
/// must match byte-for-byte or the admission is rejected with
/// `SCHEMA_MISMATCH` (see `InvariantSetResult`).
struct ChannelSchemaInvariants
{
    std::string schema_hash;
    uint32_t    schema_version{0};
    std::string schema_id;
    std::string schema_blds;
    std::string schema_owner;
};

/// Aggregate of channel-wide transport invariants supplied by REG_REQ.
/// Like `ChannelSchemaInvariants`, these are set on first admission and
/// validated equal on subsequent admissions.
struct ChannelTransportInvariants
{
    bool           has_shared_memory{false};
    std::string    shm_name;
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    data_transport{"shm"};
};

/// HEP-CORE-0036 §4.1 — per-channel access scaffolding.  Two fields:
///   - `authorized_consumer_pubkeys`: Z85 (40-char) consumer pubkeys
///     allowed to pull from this channel.  Producer's ZAP handler
///     enforces; updated via CHANNEL_AUTH_UPDATE pushes (HEP-0036 §6.5).
///     Per-channel, NOT per-producer — a consumer authorized for a
///     channel can connect to ANY producer of that channel (fan-in
///     per HEP-CORE-0023 §2.1.1).
///   - `shm_secret`: SHM-only broker-generated guard secret for the
///     DataBlock (HEP-CORE-0002).  Zero when transport != "shm".
///     Unrelated to CURVE; SHM auth uses this secret token, not
///     pubkey allowlists.
///
/// Per-producer identity (pubkey + endpoint) is NOT duplicated here —
/// it lives on `ProducerEntry::zmq_pubkey` + `zmq_node_endpoint`.
/// Channel-scope duplication would collapse fan-in.
struct ChannelAccessEntry
{
    std::unordered_set<std::string> authorized_consumer_pubkeys;
    std::uint64_t                   shm_secret{0};
};

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

/// Composite result from `HubState::_on_producer_added` — the
/// controlled-access entry point for REG_REQ admission per Wave M2.5
/// step 3 (see `docs/tech_draft/controlled_access_api_design.md` §7.5).
/// The broker handler switches on these three pieces to dispatch the
/// correct wire-error code or success response.
struct ProducerAdmissionResult
{
    /// `Created` iff the producer was appended;
    /// `RejectedUidConflict` (UID_CONFLICT wire error) or
    /// `RejectedShmCardinality` (MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM
    /// wire error) otherwise.  When this is anything other than
    /// `Created`, channel state is unchanged.
    AddProducerResult  producer_result{AddProducerResult::Created};

    /// `Created` for a fresh channel; `IdempotentEqual` when the
    /// incoming invariants match the existing channel's;
    /// `RejectedMismatch` (SCHEMA_MISMATCH / TRANSPORT_MISMATCH wire
    /// errors) when they differ.  When `RejectedMismatch`, channel
    /// state is unchanged AND `producer_result` is `Created` only by
    /// convention — the broker MUST inspect `invariant_result` first.
    InvariantSetResult invariant_result{InvariantSetResult::Created};

    /// True iff this admission opened a fresh channel record (first
    /// producer).  Caller uses this to decide whether to fire the
    /// `ch_opened` handler chain + emit HEP-CORE-0034 schema-record
    /// creation events.  False when admitting onto an existing channel.
    bool               channel_opened{false};

    /// Names which invariant didn't match, when `invariant_result ==
    /// RejectedMismatch`.  One of: `"schema_hash"`, `"schema_version"`,
    /// `"schema_id"`, `"schema_blds"`, `"schema_owner"`, `"shm_name"`,
    /// `"has_shared_memory"`, `"pattern"`, `"data_transport"`.  Empty
    /// on success.  Allows the broker to surface a specific reason
    /// (SCHEMA_MISMATCH vs TRANSPORT_MISMATCH) without re-reading
    /// HubState.
    std::string        mismatched_invariant;
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

    // Channel-scope `metadata` retired in Wave M2.5 step 6.5
    // (2026-05-10).  Per-producer metadata lives on
    // `ProducerEntry.metadata`; channel-level read aggregates them
    // via `aggregate_metadata_tree()` (per-producer tree keyed by
    // role_uid; HEP-CORE-0007 §12.4 + design §6.1).

    std::vector<ProducerEntry> producers; ///< 1..N producers (HEP-0023 §2.1.1).
    std::vector<ConsumerEntry> consumers;

    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    // zmq_ctrl_endpoint / zmq_data_endpoint retired Wave M2.5 step 2c.
    // zmq_pubkey retired Wave M2.5 step 6.5 — per-producer CURVE
    // pubkey lives on `ProducerEntry.zmq_pubkey` (HEP-CORE-0021 §5.2).
    std::string    data_transport{"shm"};
    // Channel-scope `zmq_node_endpoint` retired in Wave M2.5 step 6.5
    // (2026-05-10).  Per-producer endpoint lives on
    // `ProducerEntry.zmq_node_endpoint` (HEP-CORE-0021 §16.3).

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

    /// Set the per-producer CURVE public key (HEP-CORE-0021 §5.2).
    /// Returns true iff the producer was found.  Each Fan-In producer
    /// has its own keypair; CONSUMER_REG_ACK exposes per-producer
    /// pubkey for ZMQ ctrl socket auth.  Distinct from the deprecated
    /// channel-scope `ChannelEntry::zmq_pubkey`.
    bool set_producer_zmq_pubkey(std::string_view role_uid,
                                   std::string pubkey) noexcept
    {
        for (auto &p : producers)
        {
            if (p.role_uid == role_uid)
            {
                p.zmq_pubkey = std::move(pubkey);
                return true;
            }
        }
        return false;
    }

    /// Look up a producer's CURVE public key (empty if uid missing or
    /// pubkey not set).
    std::optional<std::string>
    producer_zmq_pubkey(std::string_view role_uid) const
    {
        for (const auto &p : producers)
            if (p.role_uid == role_uid) return p.zmq_pubkey;
        return std::nullopt;
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

// ─── Wave M3 — RoleEntry controlled-access API result types ─────────────
//
// Per `docs/tech_draft/M3_role_entry_controlled_access.md` §3 (locked
// 2026-05-11).  Mirrors the M2.5 pattern at role/presence scope:
// presence admission is purely additive (no re-add), FSM transitions
// go through typed methods, and `_set_role_disconnected` becomes
// terminal cleanup (the `disconnected_fired` memoization PATCH retires
// by construction — decision #3).

enum class AddPresenceResult
{
    Created,              ///< Appended a new RolePresence row.
    RejectedDuplicate,    ///< A presence for `(channel, role_type)` already
                          ///< exists.  Same-tuple re-add is bookkeeping
                          ///< residue — admission refused.  Caller path
                          ///< depends on context; this surface is purely
                          ///< data-structure consistency.
};

/// Coarse transition outcome from RoleEntry FSM-transition methods.
/// Mirrors HEP-CORE-0023 §2.1 state machine.  Callers that need finer-
/// grained "prev_state" detail use the richer `HeartbeatEffect` return
/// type (heartbeat-only — counters depend on prev_state).
enum class TransitionEffect
{
    NoChange,        ///< Presence not found, or already in target state.
    Refreshed,       ///< last_heartbeat updated; no FSM transition.
    NewlyConnected,  ///< First heartbeat seen, or recovery from Pending
                     ///< or Disconnected — caller reads `prev_state` on
                     ///< the result struct for counter decisions.
    ToPending,       ///< Connected → Pending (heartbeat timeout).
    ToDisconnected,  ///< Pending → Disconnected (pending timeout) or
                     ///< Connected → Disconnected (DEREG / forced).
};

/// Rich return for `RoleEntry::on_heartbeat`.  Callers that need
/// `prev_state` (e.g., to bump `pending_to_ready_total` only on a
/// Pending→Ready (= Connected post-§2) recovery) read it from here
/// directly.  The post-mutation presence state is always Connected
/// (= legacy "Ready") when `presence_found` is true — heartbeats
/// unconditionally transition to Connected per HEP-CORE-0023 §2.1.
struct HeartbeatEffect
{
    bool      presence_found{false};
    RoleState prev_state{RoleState::Disconnected};
    bool      was_first_heartbeat_seen{false};  ///< Before this heartbeat.

    /// Coarse classification for callers that only need the enum.
    TransitionEffect to_transition_effect() const noexcept
    {
        if (!presence_found) return TransitionEffect::NoChange;
        // First-heartbeat OR recovery from non-Connected.
        if (!was_first_heartbeat_seen) return TransitionEffect::NewlyConnected;
        if (prev_state != RoleState::Connected) return TransitionEffect::NewlyConnected;
        return TransitionEffect::Refreshed;
    }
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

    // `disconnected_fired` PATCH retired by Wave M3 step 4 (2026-05-11).
    // Per design decision #3: terminal-erase of `RoleEntry` on
    // last-presence-Disconnected IS the memoization.  A second call
    // to `_set_role_disconnected` finds no entry to re-fire from —
    // the structural invariant replaces the flag.  See
    // `docs/tech_draft/M3_role_entry_controlled_access.md` §6.

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

    /// Per-uid liveness.  Wave M3 step 5h (2026-05-11): after the
    /// tombstone-removal refactor, `on_dereg` and `on_pending_timeout`
    /// ERASE the presence row (rather than marking it Disconnected),
    /// so `presences[]` only contains live rows.  Liveness simplifies
    /// to "any row present" — no need to walk + check state.
    ///
    /// Per HEP-CORE-0023 §2.6 — derived, not stored.  Used by admin
    /// queries that ask "is this role still around?" and by the
    /// role-reaper (`_dispatch_role_disconnected_if_dead`) to decide
    /// whether to delete the whole `RoleEntry` when its last presence
    /// transitions Disconnected.
    [[nodiscard]] bool any_presence_alive() const noexcept
    {
        return !presences.empty();
    }

    // ── Wave M3 controlled-access API (additive step 1, 2026-05-11) ──
    //
    // Additive: methods that callers should migrate to over Wave M3
    // steps 2-5.  Existing helpers (find_presence, any_presence_alive,
    // upsert_presence_row_locked) stay; existing direct field-mutation
    // paths in hub_state.cpp also stay until the migration steps.
    // See `docs/tech_draft/M3_role_entry_controlled_access.md`.

    /// Strict additive presence admission.  Same-(channel, role_type)
    /// re-add returns `RejectedDuplicate` — a duplicate row would be
    /// bookkeeping residue (M2.5 §6.2 strict-uid policy applied at
    /// presence scope).  Caller maintains the `channels` cache
    /// invariant (decision #1, 2026-05-11): on `Created`, append
    /// the channel to `channels` if not already present.
    AddPresenceResult add_presence(std::string channel_,
                                    std::string role_type_,
                                    RolePresence **out = nullptr)
    {
        if (find_presence(channel_, role_type_) != nullptr)
            return AddPresenceResult::RejectedDuplicate;
        RolePresence p;
        p.channel              = std::move(channel_);
        p.role_type            = std::move(role_type_);
        p.state                = RoleState::Connected;
        p.first_heartbeat_seen = false;
        p.last_heartbeat       = std::chrono::steady_clock::now();
        p.state_since          = p.last_heartbeat;
        presences.push_back(std::move(p));
        if (out != nullptr) *out = &presences.back();
        return AddPresenceResult::Created;
    }

    /// Remove a presence row.  Returns true iff erased.  Caller
    /// maintains `channels` cache: drop the channel from `channels`
    /// when no other presence references it (decision #1).
    bool remove_presence(std::string_view channel_,
                          std::string_view role_type_) noexcept
    {
        for (auto it = presences.begin(); it != presences.end(); ++it)
        {
            if (it->channel == channel_ && it->role_type == role_type_)
            {
                presences.erase(it);
                return true;
            }
        }
        return false;
    }

    /// Heartbeat handler.  Updates last_heartbeat + first_heartbeat_seen;
    /// transitions FSM to Connected if not already; returns a rich
    /// `HeartbeatEffect` so callers can inspect `prev_state` for counter
    /// decisions (e.g., `pending_to_ready_total` on Pending → Ready
    /// recovery — "Ready" here is the legacy term for the Connected
    /// state post-§2 per the BrokerCounters docstring).
    /// Mutating only the matched (channel, role_type) presence — never
    /// touches sibling rows.  Per HEP-CORE-0023 §2.5.2.
    HeartbeatEffect on_heartbeat(std::string_view channel_,
                                  std::string_view role_type_,
                                  std::chrono::steady_clock::time_point when)
    {
        HeartbeatEffect r;
        for (auto &p : presences)
        {
            if (p.channel != channel_ || p.role_type != role_type_) continue;
            r.presence_found            = true;
            r.prev_state                = p.state;
            r.was_first_heartbeat_seen  = p.first_heartbeat_seen;
            p.last_heartbeat            = when;
            p.first_heartbeat_seen      = true;
            if (p.state != RoleState::Connected)
            {
                p.state       = RoleState::Connected;
                p.state_since = when;
            }
            return r;
        }
        return r;  // presence_found == false
    }

    /// Heartbeat-timeout handler.  Connected → Pending only; any other
    /// state is a no-op.  Per HEP-CORE-0023 §2.1 first-pass demotion.
    TransitionEffect on_heartbeat_timeout(std::string_view channel_,
                                           std::string_view role_type_) noexcept
    {
        for (auto &p : presences)
        {
            if (p.channel != channel_ || p.role_type != role_type_) continue;
            if (p.state != RoleState::Connected) return TransitionEffect::NoChange;
            p.state       = RoleState::Pending;
            p.state_since = std::chrono::steady_clock::now();
            return TransitionEffect::ToPending;
        }
        return TransitionEffect::NoChange;
    }

    /// Pending-timeout handler.  Pending → terminal: erases the
    /// presence row.  Wave M3 step 5h (2026-05-11) — pre-fix this
    /// marked `state = Disconnected` leaving a tombstone in
    /// `presences[]`; tombstones leaked memory for long-lived roles
    /// that churn channels.  Now the row is erased on terminal
    /// transitions, and `any_presence_alive() == !presences.empty()`.
    /// Per HEP-CORE-0023 §2.1 second-pass terminal transition.
    /// Channel teardown is HubState's responsibility — this method
    /// only deletes the presence row; the caller (`_on_pending_timeout`)
    /// runs `drop_channel_if_orphaned` next.
    TransitionEffect on_pending_timeout(std::string_view channel_,
                                         std::string_view role_type_) noexcept
    {
        for (auto it = presences.begin(); it != presences.end(); ++it)
        {
            if (it->channel != channel_ || it->role_type != role_type_) continue;
            if (it->state != RoleState::Pending) return TransitionEffect::NoChange;
            presences.erase(it);
            return TransitionEffect::ToDisconnected;
        }
        return TransitionEffect::NoChange;
    }

    /// DEREG / voluntary-disconnect handler.  Erases the presence row
    /// (Wave M3 step 5h — see `on_pending_timeout`).  Returns
    /// `ToDisconnected` iff a row was erased; `NoChange` if no
    /// matching row existed (already gone — idempotent).  Per
    /// HEP-CORE-0023 §2.1.
    TransitionEffect on_dereg(std::string_view channel_,
                               std::string_view role_type_) noexcept
    {
        for (auto it = presences.begin(); it != presences.end(); ++it)
        {
            if (it->channel != channel_ || it->role_type != role_type_) continue;
            presences.erase(it);
            return TransitionEffect::ToDisconnected;
        }
        return TransitionEffect::NoChange;
    }

    /// Wave M3 step 5d (2026-05-11) — cache invariant primitive.
    ///
    /// The `channels` member is a cache: it should contain `c` iff
    /// at least one presence row references `c` (Wave M3 decision #1).
    /// Step 5h simplified the rule to "any row references" because
    /// `on_dereg` / `on_pending_timeout` now erase rows rather than
    /// leaving Disconnected tombstones — every remaining presence is
    /// live by construction.
    ///
    /// Returns true iff the cache entry was actually removed.
    /// Idempotent: a second call (or one where the channel is already
    /// absent, or where a presence still references it) is a safe no-op.
    bool drop_channel_if_orphaned(std::string_view channel_) noexcept
    {
        for (const auto &p : presences)
        {
            if (p.channel == channel_) return false;  // still referenced
        }
        auto it = std::find(channels.begin(), channels.end(), channel_);
        if (it == channels.end()) return false;
        channels.erase(it);
        return true;
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
    // Role-presence FSM transitions (HEP-CORE-0023 §2.5).
    //
    // Naming note (audit T1, 2026-05-17): "ready" in these field names
    // is the pre-§2-rewrite term for what HEP-CORE-0023 §2.1 now calls
    // the **Connected** state (more precisely: the Connected sub-state
    // with `first_heartbeat_seen == true`, i.e. the `kLive`
    // `ChannelObservable` per §2.2).  The legacy field names are kept
    // for backward compatibility with shipped test fixtures and
    // production log scrapers — see HEP-CORE-0023 §2.5.3 for the
    // deferral note.  When reading or writing comments here, ALWAYS
    // refer to the FSM state as "Ready (= Connected post-§2)" — never
    // mix the two terms without the equivalence — so a reader is not
    // left wondering whether two terms denote the same state or two
    // different states.
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
///
/// Capture provenance metadata per HEP-CORE-0039 §3.1: `captured_at`
/// is the wall-clock timestamp, `captured_mono` is the steady-clock
/// timestamp (for `age_seconds()` math immune to NTP jumps),
/// `hub_uid` disambiguates dual-hub-processor snapshots in logs,
/// `snapshot_seq` is a per-hub monotonic counter (first live
/// snapshot has `seq == 1`; `seq == 0` is reserved for default-
/// constructed snapshots and is never returned by `HubState::snapshot()`).
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
    /// HEP-CORE-0039 §3.1 capture metadata.
    std::chrono::steady_clock::time_point         captured_mono{
        std::chrono::steady_clock::now()};
    std::string                                   hub_uid;
    std::uint64_t                                 snapshot_seq{0};
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

    /// Set the hub identifier used to stamp snapshots
    /// (`HubStateSnapshot::hub_uid`).  Called once during HubHost
    /// initialization; later snapshots carry this string verbatim.
    /// Per HEP-CORE-0039 §3.1 — disambiguates dual-hub-processor
    /// snapshots in logs.  Empty `hub_uid` is the legal "not yet
    /// initialized" state.
    void set_hub_uid(std::string hub_uid);

    // ── Read-only accessors (shared lock; copy out) ─────────────────────
    [[nodiscard]] HubStateSnapshot            snapshot() const;
    [[nodiscard]] std::optional<ChannelEntry> channel(const std::string &name) const;
    [[nodiscard]] std::optional<RoleEntry>    role(const std::string &uid) const;
    [[nodiscard]] std::optional<BandEntry>    band(const std::string &name) const;
    [[nodiscard]] std::optional<PeerEntry>    peer(const std::string &hub_uid) const;
    [[nodiscard]] std::optional<ShmBlockRef>  shm_block(const std::string &channel_name) const;
    [[nodiscard]] BrokerCounters              counters() const;

    /// HEP-CORE-0036 §4.1 — per-channel access scaffolding.  Returns
    /// the channel's `ChannelAccessEntry` if the broker has opened an
    /// access record for it (via `_on_channel_access_opened`), or
    /// `std::nullopt` if no record exists.  Broker uses this to build
    /// CONSUMER_REG_ACK + CHANNEL_AUTH_UPDATE snapshots.  Read under
    /// shared lock; value copy.
    [[nodiscard]] std::optional<ChannelAccessEntry>
    channel_access(const std::string &channel_name) const;

    /// Wave M1.4 (2026-05-11) — per-channel metrics aggregator.
    ///
    /// Reads `RolePresence::latest_metrics` from every producer and
    /// consumer presence row on the named channel and returns a JSON
    /// object of the same shape `BrokerServiceImpl::query_metrics(channel)`
    /// used to build from the legacy `metrics_store_`:
    ///
    /// ```
    /// {
    ///   "producers": { "<uid>": { ...metrics, "pid": N }, ... },
    ///   "consumers": { "<uid>": { ...metrics }, ... }
    /// }
    /// ```
    ///
    /// Empty sub-objects are omitted; the function returns an empty
    /// object if the channel does not exist or has no presences with
    /// non-null `latest_metrics`.  Metrics live on per-presence rows
    /// per HEP-CORE-0019 §2.3 Phase 6; this aggregator replaces the
    /// retired `BrokerServiceImpl::metrics_store_` path (M1.4).
    [[nodiscard]] nlohmann::json
    channel_metrics_snapshot(const std::string &channel) const;

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
    /// `reason` is the wire-protocol reason for the leave (HEP-CORE-0030
    /// BAND_LEAVE_NOTIFY): `"voluntary"` for BAND_LEAVE_REQ, `"role_closed"`
    /// for role-disconnect cascade.  Wave M3 step 5f (2026-05-11) added
    /// the reason field so the broker subscriber can fan out the wire
    /// notification with the correct reason for both paths.
    using BandLeftHandler             = std::function<void(const std::string & /*band*/,
                                                           const std::string & /*role_uid*/,
                                                           const std::string & /*reason*/)>;
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
    /// `reason` carries the wire-protocol reason ("voluntary" /
    /// "role_closed") through to the `BandLeftHandler`.  See M3 step 5f.
    void _set_band_left(const std::string &band, const std::string &role_uid,
                         const std::string &reason);
    void _set_peer_connected(PeerEntry entry);
    void _set_peer_disconnected(const std::string &hub_uid);

    /// Update one producer's `zmq_node_endpoint` on a channel
    /// (HEP-CORE-0021 §16.3 — ENDPOINT_UPDATE_REQ keyed by
    /// `(channel, role_uid)`).  Per-producer scope per Wave M2.5
    /// step 5: each Fan-In producer has its own bound endpoint.
    /// Returns true iff the channel exists AND the producer is
    /// admitted on it.  Endpoint validation is the caller's
    /// responsibility (broker handler runs `validate_tcp_endpoint`
    /// before calling this).
    bool _set_producer_zmq_node_endpoint(const std::string &channel_name,
                                          const std::string &role_uid,
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

    /// Wave M2.5 step 3 controlled-access REG_REQ admission entry point.
    /// Replaces the broker handler's prior "build fresh ChannelEntry +
    /// `_on_channel_registered`" pattern with an additive op that:
    /// - on a fresh channel: composes the channel record from the
    ///   supplied invariants + admits the producer as the first
    ///   `ProducerEntry`, fires `ch_opened` handler + `role_reg`
    ///   handler;
    /// - on an existing channel: validates invariants match the
    ///   stored record (returns `RejectedMismatch` with the
    ///   `mismatched_invariant` name set on first divergence); if
    ///   invariants match, appends the producer via
    ///   `ChannelEntry::add_producer` and fires `role_reg`.
    ///
    /// Per `docs/tech_draft/controlled_access_api_design.md` §7.5.2.
    /// `_on_channel_registered` is retained for L2 test scaffolding;
    /// production REG_REQ migrates to this op.
    ProducerAdmissionResult
    _on_producer_added(const std::string&                channel_name,
                       ChannelSchemaInvariants           schema,
                       ChannelTransportInvariants        transport,
                       ProducerEntry                     producer);

    /// Wave M2.5 step 4 controlled-access DEREG_REQ / producer-drop
    /// entry point.  Replaces `_on_channel_closed`-as-DEREG-handler:
    /// removes the matching `ProducerEntry` from
    /// `ChannelEntry.producers[]` and fires `_on_channel_closed`
    /// ONLY when the last producer leaves (HEP-CORE-0023 §2.1.1
    /// atomic teardown).
    ///
    /// Returns the typed `RemoveProducerResult` so the caller can:
    /// - `removed == false` → producer not found; surface
    ///   `NOT_REGISTERED` wire error (no state mutation).
    /// - `removed == true && !channel_now_empty` → producer dropped,
    ///   channel survives.  Caller should NOT fan-out
    ///   CHANNEL_CLOSING_NOTIFY (channel is still alive); may
    ///   optionally notify peer producers that producer X left.
    /// - `removed == true && channel_now_empty` → last producer
    ///   dropped, channel torn down.  Caller fan-out
    ///   CHANNEL_CLOSING_NOTIFY + on_channel_closed for federation
    ///   relay.  Implementation has already fired
    ///   `_on_channel_closed` internally; the channel record is
    ///   gone from `pImpl->channels` on return.
    ///
    /// Pre-conditions: writer lock taken internally.  Channel-mismatch
    /// validation done by the op-entry boundary; `reason` is logged.
    RemoveProducerResult
    _on_producer_dropped(const std::string&    channel_name,
                          const std::string&    role_uid,
                          ChannelCloseReason    reason);

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
    /// Connected → Pending transition for the `(channel, role_uid,
    /// role_type)` presence (HEP-CORE-0023 §2.1).  Producer + consumer
    /// transitions both bump `ready_to_pending_total` ("ready" is the
    /// legacy term for "Connected" — see BrokerCounters docstring);
    /// only producer transitions fan out
    /// `ChannelStatusChangedHandler` (consumer presence does not
    /// affect `ChannelObservable` per §2.1).  `role_type` MUST be
    /// `"producer"` or `"consumer"`.
    void _on_heartbeat_timeout(const std::string &channel,
                               const std::string &role_uid,
                               const std::string &role_type);
    /// Pending → Disconnected transition for the `(channel, role_uid,
    /// role_type)` presence (HEP-CORE-0023 §2.1 + §2.1.1).  Bumps
    /// `pending_to_deregistered_total`.  Behavior by role_type:
    ///
    /// - `"producer"`: drops the matching `ProducerEntry` from
    ///   `ChannelEntry.producers[]`.  Fires `_on_channel_closed`
    ///   (atomic teardown) ONLY when this was the LAST producer; the
    ///   caller inspects the returned `RemoveProducerResult` to decide
    ///   whether to fan out CHANNEL_CLOSING_NOTIFY.  Multi-producer
    ///   path: producer dropped, channel survives, role-disconnect
    ///   cascade dispatched if the role's last presence anywhere just
    ///   went Disconnected.
    /// - `"consumer"`: removes the matching `ConsumerEntry` from
    ///   `ChannelEntry.consumers[]` and runs `drop_channel_if_orphaned`
    ///   + `_dispatch_role_disconnected_if_dead`.  Consumer presence
    ///   never tears down channels (HEP-CORE-0023 §2.1.1) — the
    ///   returned `RemoveProducerResult.channel_now_empty` is always
    ///   `false` on this path.  `removed` reflects whether a consumer
    ///   slot was actually erased.
    ///
    /// Idempotent in all role_type branches: if the presence is already
    /// Disconnected or the matching channel/role row is gone, returns
    /// `{removed=false, channel_now_empty=false}` without state mutation.
    RemoveProducerResult
    _on_pending_timeout(const std::string &channel,
                         const std::string &role_uid,
                         const std::string &role_type);

    /// Wave M3 step 5b (2026-05-11): production trigger for the
    /// terminal cleanup defined at `_set_role_disconnected`.  Atomic
    /// check-and-erase under the writer lock: verify the entry
    /// exists AND `any_presence_alive() == false`, then run the
    /// same schema cascade + erase + handler fan-out as the
    /// unconditional setter.  Idempotent and TOCTOU-safe: a
    /// concurrent REG_REQ that re-armed a presence between the
    /// caller's transition and this call is observed under the
    /// same lock and is a no-op.
    ///
    /// Wired into every op that can flip the role's last alive
    /// presence to Disconnected (HEP-CORE-0023 §2.1 +
    /// HEP-CORE-0034 §7.2):
    ///   - `_on_pending_timeout`     (multi-producer path)
    ///   - `_on_consumer_left`
    ///   - `_on_channel_closed`      (per-producer)
    /// Last-producer `_on_pending_timeout` falls through to
    /// `_on_channel_closed`, which dispatches each producer uid;
    /// no duplicate fan-out (handler fires once, entry-erase
    /// guards the second call).
    ///
    /// Why a predicate-guarded helper instead of guarding
    /// `_set_role_disconnected` itself: the unconditional setter is
    /// the L2-test + admin/script force-erase entry point and must
    /// stay unconditional.
    void _dispatch_role_disconnected_if_dead(const std::string &uid);

    // M1.4 (2026-05-11): `_on_metrics_reported` deleted.  Metrics now
    // ONLY arrive via `_on_heartbeat` (HEP-CORE-0019 §2.3 Phase 6).
    // The dedicated time-only path (METRICS_REPORT_REQ) is retired.
    void _on_band_joined(const std::string &band, BandMember member);
    /// Voluntary BAND_LEAVE_REQ entry point.  Passes
    /// `reason = "voluntary"` to the `BandLeftHandler`.  Role-disconnect
    /// cascade fires the handler directly with `reason = "role_closed"`.
    void _on_band_left(const std::string &band, const std::string &role_uid);
    void _on_peer_connected(PeerEntry peer);
    void _on_peer_disconnected(const std::string &hub_uid);
    void _on_message_processed(const std::string &msg_type,
                               std::size_t        bytes_in,
                               std::size_t        bytes_out);

    // ── Channel-access capability ops (HEP-CORE-0036 §4.1) ──────────────
    //
    // The broker maintains a per-channel `ChannelAccessEntry` keyed by
    // channel name: the authorized-consumer-pubkey set + the SHM secret
    // (for SHM channels).  These four ops are the broker's write
    // path; `channel_access(name)` above is the read path.
    //
    // All four are idempotent: re-opening, re-closing, re-authorizing
    // the same pubkey, or revoking a pubkey not present is a silent
    // no-op (per HEP-0036 §I5 — security gates degrade to safe states).

    /// Open a channel-access record on producer REG_REQ accept (after
    /// `_on_producer_added` succeeded for a fresh channel).  Idempotent:
    /// no-op if a record already exists for `channel_name`.
    ///
    /// @param shm_secret Broker-generated random uint64.  Zero when
    ///                   the channel's `data_transport` != "shm".
    ///                   Non-zero for SHM channels — consumers receive
    ///                   it via `CONSUMER_REG_ACK.shm_secret` (HEP-0036
    ///                   §6.4) and pass it as the DataBlock guard token
    ///                   (HEP-CORE-0002).
    void _on_channel_access_opened(const std::string &channel_name,
                                    std::uint64_t      shm_secret);

    /// Delete a channel-access record on last-producer atomic teardown
    /// (called from the broker after `_on_channel_closed` /
    /// `_on_producer_dropped` returned `channel_now_empty == true`).
    /// Idempotent: no-op if no record exists.
    void _on_channel_access_closed(const std::string &channel_name);

    /// Add a consumer pubkey to the channel's allowlist on
    /// CONSUMER_REG_REQ accept.  Idempotent: no-op if `pubkey_z85` is
    /// already in the allowlist.  No-op if no channel-access record
    /// exists (callers should `_on_channel_access_opened` first, but
    /// out-of-order calls are silently dropped per the safe-default
    /// invariant).
    void _on_consumer_authorized(const std::string &channel_name,
                                  const std::string &pubkey_z85);

    /// Remove a consumer pubkey from the channel's allowlist on
    /// CONSUMER_DEREG_REQ / consumer-presence pending-timeout /
    /// consumer-PID-death.  Idempotent: no-op if `pubkey_z85` is not
    /// in the allowlist, or no channel-access record exists.
    void _on_consumer_revoked(const std::string &channel_name,
                               const std::string &pubkey_z85);

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
