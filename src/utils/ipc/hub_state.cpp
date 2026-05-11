/**
 * @file hub_state.cpp
 * @brief HubState — implementation of the hub's state aggregate (HEP-0033 §8).
 *
 * See hub_state.hpp for the class-level contract and phasing notes. This
 * file implements the pImpl side. Phase G2.1 ships the full surface —
 * entry maps, reader-writer concurrency, subscribe/unsubscribe, private
 * mutators that already fire events — but nothing calls into it from the
 * broker yet; that wiring lands in G2.2.
 *
 * Locking discipline
 * ------------------
 *   - `mu` (shared_mutex) guards the state maps and `counters`.
 *     Readers take `std::shared_lock`; mutators take `std::unique_lock`.
 *   - `handlers_mu` (mutex) guards the handler registries. It is
 *     independent of `mu`: subscribe/unsubscribe and event-dispatch
 *     snapshotting never hold `mu`, and mutators release `mu` before
 *     touching `handlers_mu`.
 *   - Handlers are invoked with both locks released. A handler is
 *     therefore free to read state, subscribe/unsubscribe, or call
 *     into BrokerService (which re-enters state mutators on its own
 *     thread discipline). Handlers that re-enter on the mutator
 *     thread simply observe post-mutation state.
 */
#include "utils/hub_state.hpp"

#include "utils/naming.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace pylabhub::hub
{

// ─── Enum → string helpers ──────────────────────────────────────────────────

const char *to_string(RoleState s) noexcept
{
    switch (s)
    {
    case RoleState::Connected:    return "Connected";
    case RoleState::Pending:      return "Pending";
    case RoleState::Disconnected: return "Disconnected";
    }
    return "Unknown";
}

const char *to_string(PeerState s) noexcept
{
    switch (s)
    {
    case PeerState::Connecting:   return "Connecting";
    case PeerState::Connected:    return "Connected";
    case PeerState::Disconnected: return "Disconnected";
    }
    return "Unknown";
}

const char *to_string(ChannelCloseReason r) noexcept
{
    switch (r)
    {
    case ChannelCloseReason::VoluntaryDereg:   return "VoluntaryDereg";
    case ChannelCloseReason::HeartbeatTimeout: return "HeartbeatTimeout";
    case ChannelCloseReason::AdminClose:       return "AdminClose";
    case ChannelCloseReason::BrokerShutdown:   return "BrokerShutdown";
    }
    return "Unknown";
}

const char *to_string(ChannelObservable o) noexcept
{
    switch (o)
    {
    case ChannelObservable::kAbsent:      return "absent";
    case ChannelObservable::kRegistering: return "registering";
    case ChannelObservable::kStalled:     return "stalled";
    case ChannelObservable::kLive:        return "live";
    }
    return "unknown";
}

// ─── Impl ───────────────────────────────────────────────────────────────────

struct HubState::Impl
{
    mutable std::shared_mutex mu;

    std::unordered_map<std::string, ChannelEntry> channels;
    std::unordered_map<std::string, RoleEntry>    roles;
    std::unordered_map<std::string, BandEntry>    bands;
    std::unordered_map<std::string, PeerEntry>    peers;
    std::unordered_map<std::string, ShmBlockRef>  shm_blocks;
    /// HEP-CORE-0034 §11.1 — owner-keyed schema records.
    std::map<SchemaKey, schema::SchemaRecord>     schemas;
    BrokerCounters                                counters;

    // Handler registries. Separate mutex so subscribe/unsubscribe and
    // event snapshotting are independent of state reads/writes.
    mutable std::mutex     handlers_mu;
    std::atomic<HandlerId> next_handler_id{1}; // 0 reserved for kInvalidHandlerId

    template <typename H>
    struct Slot
    {
        HandlerId id;
        H         handler;
    };

    std::vector<Slot<ChannelOpenedHandler>>        ch_opened;
    std::vector<Slot<ChannelStatusChangedHandler>> ch_status_changed;
    std::vector<Slot<ChannelClosedHandler>>        ch_closed;
    std::vector<Slot<ConsumerAddedHandler>>        cons_added;
    std::vector<Slot<ConsumerRemovedHandler>>      cons_removed;
    std::vector<Slot<RoleRegisteredHandler>>       role_reg;
    std::vector<Slot<RoleDisconnectedHandler>>     role_disc;
    std::vector<Slot<BandJoinedHandler>>           band_joined;
    std::vector<Slot<BandLeftHandler>>             band_left;
    std::vector<Slot<PeerConnectedHandler>>        peer_conn;
    std::vector<Slot<PeerDisconnectedHandler>>     peer_disc;
};

HubState::HubState() : pImpl(std::make_unique<Impl>()) {}
HubState::~HubState() = default;

// ─── Read-only accessors ────────────────────────────────────────────────────

HubStateSnapshot HubState::snapshot() const
{
    std::shared_lock lk(pImpl->mu);
    HubStateSnapshot s;
    s.channels    = pImpl->channels;
    s.roles       = pImpl->roles;
    s.bands       = pImpl->bands;
    s.peers       = pImpl->peers;
    s.shm_blocks  = pImpl->shm_blocks;
    s.schemas     = pImpl->schemas;
    s.counters    = pImpl->counters;
    s.captured_at = std::chrono::system_clock::now();
    return s;
}

std::optional<ChannelEntry> HubState::channel(const std::string &name) const
{
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->channels.find(name);
    if (it == pImpl->channels.end()) return std::nullopt;
    return it->second;
}

std::optional<RoleEntry> HubState::role(const std::string &uid) const
{
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->roles.find(uid);
    if (it == pImpl->roles.end()) return std::nullopt;
    return it->second;
}

std::optional<BandEntry> HubState::band(const std::string &name) const
{
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->bands.find(name);
    if (it == pImpl->bands.end()) return std::nullopt;
    return it->second;
}

std::optional<PeerEntry> HubState::peer(const std::string &hub_uid) const
{
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->peers.find(hub_uid);
    if (it == pImpl->peers.end()) return std::nullopt;
    return it->second;
}

std::optional<ShmBlockRef> HubState::shm_block(const std::string &channel_name) const
{
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->shm_blocks.find(channel_name);
    if (it == pImpl->shm_blocks.end()) return std::nullopt;
    return it->second;
}

BrokerCounters HubState::counters() const
{
    std::shared_lock lk(pImpl->mu);
    return pImpl->counters;
}

std::optional<schema::SchemaRecord>
HubState::schema(const std::string &owner_uid, const std::string &schema_id) const
{
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->schemas.find(SchemaKey{owner_uid, schema_id});
    if (it == pImpl->schemas.end()) return std::nullopt;
    return it->second;
}

std::size_t HubState::schema_count() const
{
    std::shared_lock lk(pImpl->mu);
    return pImpl->schemas.size();
}

// ─── Subscribe / unsubscribe ────────────────────────────────────────────────

namespace
{

template <typename Vec, typename H>
HandlerId add_handler(std::mutex &mu, std::atomic<HandlerId> &next, Vec &v, H h)
{
    std::lock_guard lk(mu);
    HandlerId       id = next.fetch_add(1, std::memory_order_relaxed);
    v.push_back({id, std::move(h)});
    return id;
}

template <typename Vec>
bool erase_handler(Vec &v, HandlerId id)
{
    auto it = std::find_if(v.begin(), v.end(),
                           [id](const auto &s) { return s.id == id; });
    if (it == v.end()) return false;
    v.erase(it);
    return true;
}

/// Snapshot handler list under handlers_mu, then return a copy. Caller
/// fires handlers with all locks released.
template <typename Vec>
auto snapshot_handlers(std::mutex &mu, const Vec &v)
{
    using HT = decltype(std::declval<typename Vec::value_type>().handler);
    std::vector<HT> out;
    std::lock_guard lk(mu);
    out.reserve(v.size());
    for (const auto &s : v) out.push_back(s.handler);
    return out;
}

} // namespace

HandlerId HubState::subscribe_channel_opened(ChannelOpenedHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->ch_opened, std::move(h));
}
HandlerId HubState::subscribe_channel_status_changed(ChannelStatusChangedHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->ch_status_changed, std::move(h));
}
HandlerId HubState::subscribe_channel_closed(ChannelClosedHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->ch_closed, std::move(h));
}
HandlerId HubState::subscribe_consumer_added(ConsumerAddedHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->cons_added, std::move(h));
}
HandlerId HubState::subscribe_consumer_removed(ConsumerRemovedHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->cons_removed, std::move(h));
}
HandlerId HubState::subscribe_role_registered(RoleRegisteredHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->role_reg, std::move(h));
}
HandlerId HubState::subscribe_role_disconnected(RoleDisconnectedHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->role_disc, std::move(h));
}
HandlerId HubState::subscribe_band_joined(BandJoinedHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->band_joined, std::move(h));
}
HandlerId HubState::subscribe_band_left(BandLeftHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->band_left, std::move(h));
}
HandlerId HubState::subscribe_peer_connected(PeerConnectedHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->peer_conn, std::move(h));
}
HandlerId HubState::subscribe_peer_disconnected(PeerDisconnectedHandler h) const
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->peer_disc, std::move(h));
}

void HubState::unsubscribe(HandlerId id) const noexcept
{
    if (id == kInvalidHandlerId) return;
    std::lock_guard lk(pImpl->handlers_mu);
    if (erase_handler(pImpl->ch_opened,         id)) return;
    if (erase_handler(pImpl->ch_status_changed, id)) return;
    if (erase_handler(pImpl->ch_closed,         id)) return;
    if (erase_handler(pImpl->cons_added,        id)) return;
    if (erase_handler(pImpl->cons_removed,      id)) return;
    if (erase_handler(pImpl->role_reg,          id)) return;
    if (erase_handler(pImpl->role_disc,         id)) return;
    if (erase_handler(pImpl->band_joined,       id)) return;
    if (erase_handler(pImpl->band_left,         id)) return;
    if (erase_handler(pImpl->peer_conn,         id)) return;
    erase_handler(pImpl->peer_disc, id);
}

// ─── Private mutators (friend-only) ─────────────────────────────────────────

void HubState::_set_channel_opened(ChannelEntry entry)
{
    ChannelEntry fired;
    {
        std::unique_lock lk(pImpl->mu);
        auto [it, _] = pImpl->channels.insert_or_assign(entry.name, std::move(entry));
        fired        = it->second;
    }
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->ch_opened)) h(fired);
}

void HubState::_set_channel_closed(const std::string &name)
{
    bool erased = false;
    {
        std::unique_lock lk(pImpl->mu);
        erased = pImpl->channels.erase(name) > 0;
    }
    if (!erased) return;
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->ch_closed)) h(name);
}

void HubState::_add_consumer(const std::string &channel, ConsumerEntry entry)
{
    ConsumerEntry fired;
    bool          ok = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel);
        if (it == pImpl->channels.end()) return;
        auto &cons = it->second.consumers;
        cons.erase(std::remove_if(cons.begin(), cons.end(),
                                  [&](const ConsumerEntry &c) {
                                      return (!entry.role_uid.empty() &&
                                              c.role_uid == entry.role_uid) ||
                                             (entry.consumer_pid != 0 &&
                                              c.consumer_pid == entry.consumer_pid);
                                  }),
                   cons.end());
        cons.push_back(std::move(entry));
        fired = cons.back();
        ok    = true;
    }
    if (!ok) return;
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->cons_added)) h(channel, fired);
}

void HubState::_remove_consumer(const std::string &channel, const std::string &role_uid)
{
    bool removed = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel);
        if (it == pImpl->channels.end()) return;
        auto &cons = it->second.consumers;
        auto  old  = cons.size();
        cons.erase(std::remove_if(cons.begin(), cons.end(),
                                  [&](const ConsumerEntry &c) { return c.role_uid == role_uid; }),
                   cons.end());
        removed = cons.size() < old;
    }
    if (!removed) return;
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->cons_removed))
        h(channel, role_uid);
}

void HubState::_set_role_registered(RoleEntry entry)
{
    RoleEntry fired;
    {
        std::unique_lock lk(pImpl->mu);
        auto [it, _] = pImpl->roles.insert_or_assign(entry.uid, std::move(entry));
        fired        = it->second;
    }
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_reg)) h(fired);
}

void HubState::_set_role_disconnected(const std::string &uid)
{
    bool        fire           = false;
    std::size_t schemas_evicted = 0;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->roles.find(uid);
        if (it == pImpl->roles.end()) return;
        // 🚧 PATCH (2026-05-10) — uses `disconnected_fired` memoization.
        // Full fix pending in Wave M2 MP3 (unified
        // `_dispatch_role_disconnected_if_dead` helper).  See
        // `docs/TODO_MASTER.md` "Wave M2".
        //
        // Idempotency: fire role_disconnected exactly once per
        // (registration → disconnect) cycle.  Memoized on
        // `RoleEntry::disconnected_fired`; cleared by revival paths
        // (upsert / heartbeat).  Event-emit memoization, not a duplicate
        // FSM state — `any_presence_alive()` remains the source of truth
        // for liveness.
        //
        // Mark all presences Disconnected (the FSM expression of "this
        // role is gone"); then, on the first call only, evict the
        // owner's schemas (HEP-CORE-0034 §7.2 — schemas evict
        // atomically with role transition to Disconnected; hub-globals
        // never touched here) and fire role_disconnected.  Schemas-
        // already-gone is a no-op; the second-call early-return avoids
        // a redundant handler invocation.
        for (auto &p : it->second.presences)
        {
            if (p.state != RoleState::Disconnected)
            {
                p.state       = RoleState::Disconnected;
                p.state_since = std::chrono::steady_clock::now();
            }
        }
        if (!it->second.disconnected_fired)
        {
            it->second.disconnected_fired = true;
            fire                          = true;
            for (auto sit = pImpl->schemas.begin(); sit != pImpl->schemas.end(); )
            {
                if (sit->first.first == uid)
                {
                    sit = pImpl->schemas.erase(sit);
                    ++schemas_evicted;
                }
                else
                {
                    ++sit;
                }
            }
            pImpl->counters.schema_evicted_total += schemas_evicted;
        }
    }
    if (!fire) return;
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_disc)) h(uid);
}

void HubState::_set_band_joined(const std::string &band, BandMember member)
{
    BandMember fired;
    {
        std::unique_lock lk(pImpl->mu);
        auto &           b = pImpl->bands[band];
        if (b.name.empty()) b.name = band;
        auto &m = b.members;
        m.erase(std::remove_if(m.begin(), m.end(),
                               [&](const BandMember &x) { return x.role_uid == member.role_uid; }),
                m.end());
        m.push_back(std::move(member));
        b.last_activity = std::chrono::steady_clock::now();
        fired           = m.back();
    }
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->band_joined)) h(band, fired);
}

void HubState::_set_band_left(const std::string &band, const std::string &role_uid)
{
    bool fire = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->bands.find(band);
        if (it == pImpl->bands.end()) return;
        auto &m   = it->second.members;
        auto  old = m.size();
        m.erase(std::remove_if(m.begin(), m.end(),
                               [&](const BandMember &x) { return x.role_uid == role_uid; }),
                m.end());
        fire                      = m.size() < old;
        it->second.last_activity  = std::chrono::steady_clock::now();
        if (m.empty()) pImpl->bands.erase(it);
    }
    if (!fire) return;
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->band_left)) h(band, role_uid);
}

void HubState::_set_peer_connected(PeerEntry entry)
{
    PeerEntry fired;
    {
        std::unique_lock lk(pImpl->mu);
        const std::string uid = entry.uid;
        entry.state           = PeerState::Connected;
        entry.last_seen       = std::chrono::steady_clock::now();
        auto [it, _]          = pImpl->peers.insert_or_assign(uid, std::move(entry));
        fired                 = it->second;
    }
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->peer_conn)) h(fired);
}

void HubState::_set_peer_disconnected(const std::string &hub_uid)
{
    bool fire = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->peers.find(hub_uid);
        if (it == pImpl->peers.end()) return;
        if (it->second.state != PeerState::Disconnected)
        {
            it->second.state = PeerState::Disconnected;
            fire             = true;
        }
    }
    if (!fire) return;
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->peer_disc)) h(hub_uid);
}

void HubState::_set_channel_zmq_node_endpoint(const std::string &name,
                                              std::string        endpoint)
{
    // Wave M2.5 step 5 — this op is now a thin shim over the
    // per-producer op (the channel-scope concept is retired).  Targets
    // the FIRST producer for backwards compat with any caller not yet
    // migrated to `_set_producer_zmq_node_endpoint`.  The broker's
    // ENDPOINT_UPDATE_REQ handler routes directly to the per-producer
    // op via the sender's resolved role_uid.
    std::unique_lock lk(pImpl->mu);
    auto             it = pImpl->channels.find(name);
    if (it == pImpl->channels.end()) return;
    if (auto *p = it->second.first_producer())
    {
        p->zmq_node_endpoint = std::move(endpoint);
    }
}

bool HubState::_set_producer_zmq_node_endpoint(const std::string &channel_name,
                                                const std::string &role_uid,
                                                std::string        endpoint)
{
    // Per-producer endpoint update (HEP-CORE-0021 §16.3).  Identifier
    // validation is the caller's responsibility (broker
    // ENDPOINT_UPDATE_REQ handler runs the channel+uid checks before
    // calling this — same convention as `_set_channel_zmq_node_endpoint`).
    if (channel_name.empty() || role_uid.empty() || endpoint.empty())
        return false;
    std::unique_lock lk(pImpl->mu);
    auto             it = pImpl->channels.find(channel_name);
    if (it == pImpl->channels.end()) return false;
    return it->second.set_producer_zmq_node_endpoint(role_uid, std::move(endpoint));
}

void HubState::_set_shm_block(ShmBlockRef ref)
{
    std::unique_lock lk(pImpl->mu);
    pImpl->shm_blocks.insert_or_assign(ref.channel_name, std::move(ref));
}

void HubState::_bump_msg_type_error(const std::string &msg_type, uint64_t n)
{
    std::unique_lock lk(pImpl->mu);
    pImpl->counters.msg_type_errors[msg_type] += n;
}

void HubState::_bump_counter(const std::string &key, uint64_t n)
{
    std::unique_lock lk(pImpl->mu);
    pImpl->counters.msg_type_counts[key] += n;
}

// ─── Capability-operation layer (HEP-0033 §G2) ──────────────────────────────
//
// Each `_on_*` composes primitive `_set_*` setters to represent one
// hub-level event atomically at the semantic layer ("a channel was
// registered") even though under the hood it touches multiple maps.
// See hub_state.hpp for the atomicity note (field-level atomicity
// preserved; multi-map composition is sequential, matching today's
// broker behavior).

namespace
{

/// Read-modify-write upsert for a RoleEntry, preserving the existing
/// first_seen + channels list when the role already exists.
///
/// Preconditions (caller-enforced): `uid` is a well-formed RoleUid
/// per HEP-0033 §G2.2.0b.  This helper assumes validation has
/// already happened at the `_on_*` op entry and derives `role.name`
/// and `role.role_tag` from the uid via `parse_role_uid()`.
///
/// Callers must hold no lock on entry; this helper takes the state
/// writer lock briefly and returns the post-mutation entry for event
/// dispatch.
/// Insert-or-update a `RolePresence` row on @p role.  Idempotent: if the
/// `(channel, role_type)` row already exists, it is left untouched
/// (re-registration after voluntary close is handled by the heartbeat
/// path which transitions Disconnected → Connected when a fresh
/// heartbeat arrives).  Pure helper — caller must hold the impl mutex.
inline void upsert_presence_row_locked(
    RoleEntry          &role,
    const std::string  &channel,
    const std::string  &role_type,
    std::chrono::steady_clock::time_point now)
{
    if (channel.empty() || role_type.empty()) return;
    if (role.find_presence(channel, role_type) != nullptr) return;
    RolePresence p;
    p.channel               = channel;
    p.role_type             = role_type;
    p.state                 = RoleState::Connected;
    p.first_heartbeat_seen  = false;          // "registering" sub-state
    p.last_heartbeat        = now;            // re-stamped on first HB
    p.state_since           = now;
    role.presences.push_back(std::move(p));
}

template <typename Impl>
RoleEntry upsert_role_locked(Impl &impl, const std::string &uid,
                             const std::string &pubkey_z85,
                             const std::string &added_channel,
                             const std::string &role_type,
                             std::chrono::steady_clock::time_point heartbeat_when)
{
    // Derived fields from uid — uid is the source of truth for tag/name
    // per HEP-0033 §G2.2.0b "UID construction".
    const auto parts = parse_role_uid(uid);
    // parse_role_uid failing here would be a caller-contract violation
    // (op-entry validation should have caught it). Treat as defensive
    // no-op: return an empty placeholder; the mutator's null-uid guard
    // is the first line of defense.
    if (!parts) return {};

    const std::string derived_tag (parts->tag);
    const std::string derived_name(parts->name);

    std::unique_lock lk(impl.mu);
    auto             it = impl.roles.find(uid);
    if (it == impl.roles.end())
    {
        RoleEntry r;
        r.uid        = uid;
        r.name       = derived_name;
        r.role_tag   = derived_tag;
        r.pubkey_z85 = pubkey_z85;
        if (!added_channel.empty()) r.channels.push_back(added_channel);
        // Eager presence creation per HEP-CORE-0023 §2.6: the presence
        // row is created at REG time so DISC_REQ before the first
        // heartbeat resolves to "registering" (`!first_heartbeat_seen`)
        // → DISC_PENDING.
        upsert_presence_row_locked(r, added_channel, role_type, heartbeat_when);
        auto [new_it, _] = impl.roles.emplace(uid, std::move(r));
        return new_it->second;
    }
    auto &ex = it->second;
    // name + role_tag are derived caches — re-derive on every upsert
    // so the invariant role_tag == extract_role_tag(uid) always holds.
    ex.name     = derived_name;
    ex.role_tag = derived_tag;
    if (!pubkey_z85.empty()) ex.pubkey_z85 = pubkey_z85;
    if (!added_channel.empty() &&
        std::find(ex.channels.begin(), ex.channels.end(), added_channel) ==
            ex.channels.end())
    {
        ex.channels.push_back(added_channel);
    }
    upsert_presence_row_locked(ex, added_channel, role_type, heartbeat_when);
    // 🚧 PATCH (2026-05-10) — full fix pending in Wave M2 MP3.
    // Revival: the role is being re-touched (REG_REQ, CONSUMER_REG_REQ,
    // or band-join after a prior disconnect).  Reset the event-emit
    // memoization so a future disconnect can fire `role_disconnected`
    // again.
    ex.disconnected_fired = false;
    return ex;
}

/// Bumps the sys.invalid_identifier_rejected counter without taking
/// any public mutator path (those might themselves run validation).
/// Used by `_on_*` ops when they silent-drop on malformed input.
template <typename Impl>
void bump_invalid_identifier(Impl &impl) noexcept
{
    std::unique_lock lk(impl.mu);
    ++impl.counters.msg_type_counts["sys.invalid_identifier_rejected"];
}

} // namespace

void HubState::_on_channel_registered(ChannelEntry entry)
{
    // Validate at the op-entry boundary (HEP-0033 §G2.2.0b). Invalid
    // identifiers result in a silent drop + sys.invalid_identifier_rejected
    // counter bump; the wire-handler layer (G2.2.1+) is responsible for
    // returning an explicit error to the client.
    if (!is_valid_identifier(entry.name, IdentifierKind::Channel))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    // Validate every `role_uid` in `entry.producers` per HEP-CORE-0023
    // §2.1.1 (multi-producer model).  Wave M2.5 step 3 (commit
    // ed15f02) migrated the broker REG_REQ handler to
    // `_on_producer_added` — production no longer reaches this op.
    // L2 test scaffolding (`HubStateTestAccess::on_channel_registered`)
    // continues to use it; the input `entry.producers` may still
    // contain a single producer per test call.
    for (const auto &prod : entry.producers)
    {
        if (!prod.role_uid.empty() &&
            !is_valid_identifier(prod.role_uid, IdentifierKind::RoleUid))
        {
            bump_invalid_identifier(*pImpl);
            return;
        }
    }
    // schema_id is optional — validate only when set.  An old-format
    // '<base>@<version>' schema_id would fail `Schema` grammar and get
    // silent-dropped here, surfacing bugs at the HubState boundary
    // (wire-handler layer G2.2.1+ will also reject with an error reply).
    if (!entry.schema_id.empty() &&
        !is_valid_identifier(entry.schema_id, IdentifierKind::Schema))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }

    // Capture pieces needed for role/shm derivation before moving entry.
    // This is the test-only legacy primitive path: callers pass an
    // entry with one ProducerEntry per call.  Production REG_REQ
    // routes through `_on_producer_added` (Wave M2.5 step 3); that
    // op creates role/presence per admitted ProducerEntry.
    const std::string channel_name    = entry.name;
    const std::string producer_uid    = entry.producers.empty()
                                           ? std::string{}
                                           : entry.producers.front().role_uid;
    const std::string producer_pubkey = entry.zmq_pubkey;
    const bool        has_shm         = entry.has_shared_memory;
    const std::string shm_name        = entry.shm_name;

    _set_channel_opened(std::move(entry));

    if (!producer_uid.empty())
    {
        RoleEntry fired = upsert_role_locked(
            *pImpl, producer_uid, producer_pubkey, channel_name,
            /*role_type=*/"producer",
            std::chrono::steady_clock::now());
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_reg))
            h(fired);
    }

    if (has_shm)
        _set_shm_block(ShmBlockRef{channel_name, shm_name});

}

// Wave M2.5 step 3 — additive producer admission op.  See
// `docs/tech_draft/controlled_access_api_design.md` §7.5.2.
ProducerAdmissionResult
HubState::_on_producer_added(const std::string&         channel_name,
                              ChannelSchemaInvariants    schema,
                              ChannelTransportInvariants transport,
                              ProducerEntry              producer)
{
    ProducerAdmissionResult result;

    // Identifier validation at the op-entry boundary (mirrors
    // `_on_channel_registered`).  Invalid identifiers bump
    // sys.invalid_identifier_rejected and silent-drop; the wire layer
    // returns a typed error.
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel))
    {
        bump_invalid_identifier(*pImpl);
        result.invariant_result = InvariantSetResult::RejectedMismatch;
        result.mismatched_invariant = "channel_name";
        return result;
    }
    if (!producer.role_uid.empty() &&
        !is_valid_identifier(producer.role_uid, IdentifierKind::RoleUid))
    {
        bump_invalid_identifier(*pImpl);
        result.producer_result = AddProducerResult::RejectedUidConflict;
        return result;
    }
    if (!schema.schema_id.empty() &&
        !is_valid_identifier(schema.schema_id, IdentifierKind::Schema))
    {
        bump_invalid_identifier(*pImpl);
        result.invariant_result = InvariantSetResult::RejectedMismatch;
        result.mismatched_invariant = "schema_id";
        return result;
    }

    // Capture pieces needed for role/shm side-effects before taking the
    // writer lock (so we don't hold the writer lock across handler
    // dispatch).  Use the producer's own pubkey now (M2.5 step 3 — the
    // pubkey lives per-producer per HEP-CORE-0021 §5.2).
    const std::string producer_uid     = producer.role_uid;
    const std::string producer_pubkey  = producer.zmq_pubkey;
    const bool        has_shm          = transport.has_shared_memory;
    const std::string shm_name         = transport.shm_name;

    ChannelEntry fired_entry;
    bool         did_fire_open = false;

    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel_name);

        if (it == pImpl->channels.end())
        {
            // Fresh channel — first producer.  Compose the record from
            // the supplied invariants + producer, insert, then fire
            // ch_opened outside the lock.
            ChannelEntry entry;
            entry.name              = channel_name;
            entry.schema_hash       = schema.schema_hash;
            entry.schema_version    = schema.schema_version;
            entry.schema_id         = schema.schema_id;
            entry.schema_blds       = schema.schema_blds;
            entry.schema_owner      = schema.schema_owner;
            entry.has_shared_memory = transport.has_shared_memory;
            entry.shm_name          = transport.shm_name;
            entry.pattern           = transport.pattern;
            entry.data_transport    = transport.data_transport;
            // Append the producer via the controlled API (Created by
            // construction since producers is empty + we just checked
            // shm cardinality is fine for the first entry).  Capture
            // the typed result for the caller's switch.
            result.producer_result  = entry.add_producer(std::move(producer));
            if (result.producer_result != AddProducerResult::Created)
            {
                // Shouldn't happen on a fresh entry (no uid conflict
                // possible, SHM cardinality is 1<=1), but if it does
                // we leave channel unopened and propagate the result.
                return result;
            }
            auto [new_it, _] = pImpl->channels.insert_or_assign(
                channel_name, std::move(entry));
            fired_entry             = new_it->second;
            did_fire_open           = true;
            result.invariant_result = InvariantSetResult::Created;
            result.channel_opened   = true;
        }
        else
        {
            // Existing channel — validate invariants match.  Return on
            // first mismatch so the caller can surface a specific code
            // (SCHEMA_MISMATCH for schema_*; TRANSPORT_MISMATCH for
            // has_shared_memory / data_transport / pattern / shm_name).
            const auto &cur = it->second;
            auto reject = [&](const char *name) {
                result.invariant_result     = InvariantSetResult::RejectedMismatch;
                result.mismatched_invariant = name;
            };
            if (cur.schema_hash    != schema.schema_hash)    { reject("schema_hash");    return result; }
            if (cur.schema_version != schema.schema_version) { reject("schema_version"); return result; }
            if (cur.schema_id      != schema.schema_id)      { reject("schema_id");      return result; }
            if (cur.schema_blds    != schema.schema_blds)    { reject("schema_blds");    return result; }
            if (cur.schema_owner   != schema.schema_owner)   { reject("schema_owner");   return result; }
            if (cur.has_shared_memory != transport.has_shared_memory)
                                                              { reject("has_shared_memory"); return result; }
            if (cur.shm_name       != transport.shm_name)    { reject("shm_name");       return result; }
            if (cur.pattern        != transport.pattern)     { reject("pattern");        return result; }
            if (cur.data_transport != transport.data_transport)
                                                              { reject("data_transport"); return result; }

            // Invariants match — append the producer via the controlled
            // API.  Typed result (Created / RejectedUidConflict /
            // RejectedShmCardinality) propagates straight through.
            result.invariant_result = InvariantSetResult::IdempotentEqual;
            result.producer_result  = it->second.add_producer(std::move(producer));
            if (result.producer_result != AddProducerResult::Created)
            {
                // No state change on either reject; return as-is.
                return result;
            }
            result.channel_opened = false;
        }
    } // release writer lock before firing handlers.

    if (did_fire_open)
    {
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->ch_opened))
            h(fired_entry);
    }

    if (!producer_uid.empty())
    {
        RoleEntry fired = upsert_role_locked(
            *pImpl, producer_uid, producer_pubkey, channel_name,
            /*role_type=*/"producer",
            std::chrono::steady_clock::now());
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_reg))
            h(fired);
    }

    if (did_fire_open && has_shm)
        _set_shm_block(ShmBlockRef{channel_name, shm_name});

    return result;
}

// Wave M2.5 step 4 — additive producer-drop op.  See
// `docs/tech_draft/controlled_access_api_design.md` §7 step 4 +
// HEP-CORE-0023 §2.1.1 atomic teardown rule.
RemoveProducerResult
HubState::_on_producer_dropped(const std::string& channel_name,
                                const std::string& role_uid,
                                ChannelCloseReason reason)
{
    RemoveProducerResult result{false, false};

    // Identifier validation at the op-entry boundary.
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel) ||
        !is_valid_identifier(role_uid,    IdentifierKind::RoleUid))
    {
        bump_invalid_identifier(*pImpl);
        return result;
    }

    bool is_last_producer = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel_name);
        if (it == pImpl->channels.end()) return result;  // removed=false

        // Probe before mutating: is the uid registered, and is it the
        // LAST producer on this channel?  This ordering matters for
        // the cascade eviction in `_on_channel_closed`: that function
        // captures `producer_uids` by reading the channel's
        // `producers[]` list to evict each owner's schemas (HEP-
        // CORE-0034 §7.2).  If we removed the producer FIRST and then
        // called `_on_channel_closed`, the producers list would be
        // empty and no schemas would be evicted.  Two cases:
        //
        //  - Non-last (>=2 producers remain): remove now; channel
        //    survives; return.
        //  - Last (this drop empties the channel): leave the producer
        //    in the list and call `_on_channel_closed` below, which
        //    captures the uid + runs the schema cascade + erases the
        //    channel record (including the lingering producer).
        if (it->second.find_producer(role_uid) == nullptr) return result;
        is_last_producer = (it->second.producer_count() == 1);
        if (!is_last_producer)
        {
            auto rm = it->second.remove_producer(role_uid);
            result.removed             = rm.removed;
            result.channel_now_empty   = false;  // by construction
            return result;
        }
        // Last-producer path: fall through to _on_channel_closed
        // (after lock release).  The producer stays in producers[]
        // until then so the cascade can see it.
    }

    // Last-producer path: atomic teardown per HEP-CORE-0023 §2.1.1.
    // `_on_channel_closed` re-takes the writer lock; reads producers[]
    // (with our uid still admitted); runs the schema-record cascade
    // (HEP-CORE-0034 §7.2); fires `ch_closed` handler; erases the
    // channel record.  Producer-presence row on RoleEntry stays until
    // heartbeat-timeout / DEREG of the presence (Wave M3 routes this
    // through the RoleEntry controlled-access API).
    _on_channel_closed(channel_name, reason);
    result.removed           = true;
    result.channel_now_empty = true;
    return result;
}

void HubState::_on_channel_closed(const std::string &name, ChannelCloseReason why)
{
    if (!is_valid_identifier(name, IdentifierKind::Channel))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }

    // HEP-CORE-0034 §7.2 — capture every producer uid BEFORE erasing the
    // channel, so the cascade evictions below can iterate them.  In the
    // multi-producer model (HEP-CORE-0023 §2.1.1) a channel can have
    // 1..N producers; when channel-close fires (atomic teardown when the
    // LAST live producer-presence transitions Disconnected, or
    // voluntary admin/script close), every producer's
    // private schema records must be evicted.  Hub-globals
    // (owner=="hub") are immune.
    std::vector<std::string> producer_uids;
    {
        std::shared_lock rlk(pImpl->mu);
        auto it = pImpl->channels.find(name);
        if (it != pImpl->channels.end())
        {
            producer_uids.reserve(it->second.producers.size());
            for (const auto &p : it->second.producers)
                if (!p.role_uid.empty()) producer_uids.push_back(p.role_uid);
        }
    }

    _set_channel_closed(name);
    // Remove channel from each role's channels list and atomically mark
    // every producer-presence on this channel as Disconnected
    // (HEP-CORE-0023 §2.1.1 atomic teardown — when the channel is gone,
    // all its producer-presences are terminal).  Other presences
    // belonging to the same role on different channels are left alone.
    {
        std::unique_lock lk(pImpl->mu);
        for (auto &[uid, role] : pImpl->roles)
        {
            auto it = std::find(role.channels.begin(), role.channels.end(), name);
            if (it != role.channels.end()) role.channels.erase(it);
        }
        for (const auto &producer_uid : producer_uids)
        {
            auto rit = pImpl->roles.find(producer_uid);
            if (rit == pImpl->roles.end()) continue;
            if (auto *p = rit->second.find_presence(name, "producer");
                p != nullptr && p->state != RoleState::Disconnected)
            {
                p->state       = RoleState::Disconnected;
                p->state_since = std::chrono::steady_clock::now();
            }
        }
        pImpl->shm_blocks.erase(name);
    }
    _bump_counter(std::string("close:") + to_string(why));

    // Cascade schema eviction — outside any lock held above
    // (`_on_schemas_evicted_for_owner` takes its own writer lock).
    // Wave M3 (RoleEntry controlled-access API) will move this to the
    // role-disconnect cascade (HEP-CORE-0034 §7.2) so eviction follows
    // owner-lifetime rather than channel-close; until then, evict
    // per-producer here.
    for (const auto &producer_uid : producer_uids)
        _on_schemas_evicted_for_owner(producer_uid);
}

void HubState::_on_consumer_joined(const std::string &channel, ConsumerEntry consumer)
{
    if (!is_valid_identifier(channel, IdentifierKind::Channel))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    if (!consumer.role_uid.empty() &&
        !is_valid_identifier(consumer.role_uid, IdentifierKind::RoleUid))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }

    const std::string consumer_uid = consumer.role_uid;

    _add_consumer(channel, std::move(consumer));

    if (!consumer_uid.empty())
    {
        RoleEntry fired = upsert_role_locked(
            *pImpl, consumer_uid, /*pubkey*/ {}, channel,
            /*role_type=*/"consumer",
            std::chrono::steady_clock::now());
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_reg))
            h(fired);
    }

}

void HubState::_on_consumer_left(const std::string &channel, const std::string &role_uid)
{
    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        (!role_uid.empty() && !is_valid_identifier(role_uid, IdentifierKind::RoleUid)))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    _remove_consumer(channel, role_uid);
    // Remove this channel from the role's channels list and mark the
    // consumer-presence Disconnected (HEP-CORE-0023 §2.1).  Do NOT mark
    // the whole role disconnected — the role may still be producing on
    // other channels or attached as a consumer elsewhere.
    if (!role_uid.empty())
    {
        std::unique_lock lk(pImpl->mu);
        auto it = pImpl->roles.find(role_uid);
        if (it != pImpl->roles.end())
        {
            auto &chs = it->second.channels;
            auto  rm  = std::find(chs.begin(), chs.end(), channel);
            if (rm != chs.end()) chs.erase(rm);
            if (auto *p = it->second.find_presence(channel, "consumer");
                p != nullptr && p->state != RoleState::Disconnected)
            {
                p->state       = RoleState::Disconnected;
                p->state_since = std::chrono::steady_clock::now();
            }
        }
    }
}

void HubState::_on_heartbeat(const std::string                   &channel,
                             const std::string                   &role_uid,
                             const std::string                   &role_type,
                             std::chrono::steady_clock::time_point when,
                             const std::optional<nlohmann::json> &metrics)
{
    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        (!role_uid.empty() && !is_valid_identifier(role_uid, IdentifierKind::RoleUid)))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    // Per HEP-CORE-0023 §2.5.2: each heartbeat refreshes ONLY the
    // matching `(uid, channel, role_type)` presence row.  Presence
    // rows are created eagerly at REG time (§2.6); a heartbeat
    // for an unknown presence is a no-op.
    if (role_uid.empty() || role_type.empty()) return;

    bool              observable_changed = false;
    ChannelEntry      fired_entry;
    ChannelObservable new_obs = ChannelObservable::kAbsent;
    {
        std::unique_lock lk(pImpl->mu);
        auto             rit = pImpl->roles.find(role_uid);
        if (rit == pImpl->roles.end()) return;
        auto *p = rit->second.find_presence(channel, role_type);
        if (p == nullptr) return;

        const bool      was_first = !p->first_heartbeat_seen;
        const RoleState was_state = p->state;

        p->last_heartbeat       = when;
        p->first_heartbeat_seen = true;
        if (p->state != RoleState::Connected)
        {
            const RoleState prev = p->state;
            p->state             = RoleState::Connected;
            p->state_since       = when;
            // Recovery from Pending counts as pending_to_ready
            // (HEP-CORE-0023 §2.5).
            if (prev == RoleState::Pending)
                ++pImpl->counters.pending_to_ready_total;
            // 🚧 PATCH (2026-05-10) — full fix pending in Wave M2 MP3.
            // Revival: clear the role's role_disconnected-already-fired
            // memoization so a future disconnect can re-emit.
            rit->second.disconnected_fired = false;
        }
        if (metrics.has_value())
        {
            p->latest_metrics       = *metrics;
            p->metrics_collected_at = std::chrono::system_clock::now();
        }

        // Fire ChannelStatusChangedHandler when the producer-presence
        // transition flips the channel's observable
        // (kRegistering→kLive on first heartbeat; kStalled→kLive on
        // recovery from Pending; kAbsent→kLive on revival from
        // Disconnected).  Consumer-presence transitions are not
        // visible on the channel observable.
        if (role_type == "producer" && (was_first || was_state != RoleState::Connected))
        {
            auto cit = pImpl->channels.find(channel);
            if (cit != pImpl->channels.end())
            {
                fired_entry        = cit->second;
                new_obs            = compute_channel_observable(
                                         cit->second, pImpl->roles);
                observable_changed = true;
            }
        }
    }
    if (observable_changed)
    {
        for (auto &h : snapshot_handlers(pImpl->handlers_mu,
                                         pImpl->ch_status_changed))
            h(fired_entry, new_obs);
    }
}

void HubState::_on_heartbeat_timeout(const std::string &channel,
                                     const std::string &role_uid)
{
    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        (!role_uid.empty() && !is_valid_identifier(role_uid, IdentifierKind::RoleUid)))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    // HEP-CORE-0023 §2.1: producer-presence Connected → Pending.  The
    // role is NOT marked Disconnected here — Pending means "suspicious,
    // may recover via the next heartbeat".  Disconnection (if any)
    // happens at `_on_pending_timeout`.
    bool              transitioned = false;
    ChannelEntry      fired_entry;
    ChannelObservable new_obs = ChannelObservable::kAbsent;
    {
        std::unique_lock lk(pImpl->mu);
        const auto       now = std::chrono::steady_clock::now();
        if (role_uid.empty()) return;
        auto rit = pImpl->roles.find(role_uid);
        if (rit == pImpl->roles.end()) return;
        auto *p = rit->second.find_presence(channel, "producer");
        if (p == nullptr || p->state != RoleState::Connected) return;

        // `first_heartbeat_seen` is NOT a gate — the registered-but-
        // never-heartbeat case demotes via this same path once
        // `last_heartbeat` (stamped at REG_REQ time) ages past
        // ready_timeout.
        p->state       = RoleState::Pending;
        p->state_since = now;
        ++pImpl->counters.ready_to_pending_total;
        transitioned = true;

        auto it = pImpl->channels.find(channel);
        if (it != pImpl->channels.end())
        {
            fired_entry = it->second;
            new_obs     = compute_channel_observable(it->second, pImpl->roles);
        }
    }
    if (transitioned)
    {
        for (auto &h : snapshot_handlers(pImpl->handlers_mu,
                                         pImpl->ch_status_changed))
            h(fired_entry, new_obs);
    }
}

RemoveProducerResult
HubState::_on_pending_timeout(const std::string &channel,
                               const std::string &role_uid)
{
    RemoveProducerResult result{false, false};

    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        !is_valid_identifier(role_uid, IdentifierKind::RoleUid))
    {
        bump_invalid_identifier(*pImpl);
        return result;
    }
    // HEP-CORE-0023 §2.1 + §2.1.1: producer-presence Pending →
    // Disconnected; atomic channel teardown fires ONLY on the LAST
    // producer's transition.  No grace window, no Closing state.
    //
    // The producer-presence's `Pending` state is the single-shot gate
    // — a concurrent timer fire that loses the writer-lock race
    // observes Disconnected (or no presence) and bails without
    // re-entering teardown or double-bumping the counter.
    //
    // Ordering note: we transition the presence FSM AND probe whether
    // this is the last producer BEFORE removing the producer from
    // `ChannelEntry.producers[]`.  Removal-before-close-cascade would
    // empty `producers[]` and skip schema eviction in
    // `_on_channel_closed` (same bug class as Wave M2.5 step 4 fix).
    bool is_last_producer = false;
    bool eligible         = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel);
        if (it == pImpl->channels.end()) return result;
        if (it->second.find_producer(role_uid) == nullptr) return result;

        auto rit = pImpl->roles.find(role_uid);
        if (rit == pImpl->roles.end()) return result;
        auto *p = rit->second.find_presence(channel, "producer");
        if (p == nullptr || p->state != RoleState::Pending) return result;

        p->state       = RoleState::Disconnected;
        p->state_since = std::chrono::steady_clock::now();
        ++pImpl->counters.pending_to_deregistered_total;
        eligible         = true;
        is_last_producer = (it->second.producer_count() == 1);
        if (!is_last_producer)
        {
            // Multi-producer channel — drop just this one; channel
            // survives.  Producer-presence FSM already transitioned
            // above; the row stays on RoleEntry (Wave M3 routes
            // cleanup through the RoleEntry API).
            auto rm                  = it->second.remove_producer(role_uid);
            result.removed           = rm.removed;
            result.channel_now_empty = false;
            return result;
        }
        // Last-producer path: leave the producer in producers[] so
        // _on_channel_closed's cascade eviction can read its uid.
    }

    if (eligible)
    {
        _on_channel_closed(channel, ChannelCloseReason::HeartbeatTimeout);
        result.removed           = true;
        result.channel_now_empty = true;
    }
    return result;
}

void HubState::_on_metrics_reported(const std::string                    &channel,
                                    const std::string                    &role_uid,
                                    nlohmann::json                        metrics,
                                    std::chrono::system_clock::time_point when)
{
    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        (!role_uid.empty() && !is_valid_identifier(role_uid, IdentifierKind::RoleUid)))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    // HEP-0033 §9.1 "Metrics report tick" — dedicated METRICS_REPORT_REQ
    // path.  No liveness effect: a role could be stalled on the heartbeat
    // axis but still drip metrics on this channel.  Metrics live on the
    // per-presence row (HEP-CORE-0019 §2.3); we update every presence
    // matching `(channel, role_uid)` — typically exactly one row, but a
    // role registered on the same channel as both producer AND consumer
    // would have two.
    if (role_uid.empty()) return;
    std::unique_lock lk(pImpl->mu);
    auto             rit = pImpl->roles.find(role_uid);
    if (rit == pImpl->roles.end()) return;
    for (auto &p : rit->second.presences)
    {
        if (p.channel == channel)
        {
            p.latest_metrics       = metrics;
            p.metrics_collected_at = when;
        }
    }
}

void HubState::_on_band_joined(const std::string &band, BandMember member)
{
    if (!is_valid_identifier(band, IdentifierKind::Band) ||
        (!member.role_uid.empty() &&
         !is_valid_identifier(member.role_uid, IdentifierKind::RoleUid)))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }

    const std::string role_uid = member.role_uid;

    _set_band_joined(band, std::move(member));

    if (!role_uid.empty())
    {
        RoleEntry fired = upsert_role_locked(
            *pImpl, role_uid, /*pubkey*/ {}, /*channel*/ {},
            /*role_type=*/{},   // band membership; no presence row
            std::chrono::steady_clock::now());
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_reg))
            h(fired);
    }

}

void HubState::_on_band_left(const std::string &band, const std::string &role_uid)
{
    if (!is_valid_identifier(band, IdentifierKind::Band) ||
        (!role_uid.empty() && !is_valid_identifier(role_uid, IdentifierKind::RoleUid)))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    _set_band_left(band, role_uid);
}

void HubState::_on_peer_connected(PeerEntry peer)
{
    if (!is_valid_identifier(peer.uid, IdentifierKind::PeerUid))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    _set_peer_connected(std::move(peer));
}

void HubState::_on_peer_disconnected(const std::string &hub_uid)
{
    if (!is_valid_identifier(hub_uid, IdentifierKind::PeerUid))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    _set_peer_disconnected(hub_uid);
}

void HubState::_on_message_processed(const std::string &msg_type,
                                     std::size_t        bytes_in,
                                     std::size_t        bytes_out)
{
    std::unique_lock lk(pImpl->mu);
    ++pImpl->counters.msg_type_counts[msg_type];
    pImpl->counters.bytes_in_total  += bytes_in;
    pImpl->counters.bytes_out_total += bytes_out;
}

// ─── Schema-registry capability ops (HEP-CORE-0034 §11) ─────────────────────

schema::SchemaRegOutcome HubState::_on_schema_registered(schema::SchemaRecord rec)
{
    using O = schema::SchemaRegOutcome;

    // HubState-local invariants only (Phase 2):
    //   - Owner uid must be present.  Distinguishing "is this caller
    //     allowed to claim this owner?" requires broker-side role context
    //     (Phase 3), so we only catch the empty-string case here.
    //   - Schema id must be present (defensive — REG_REQ shape check
    //     should catch this earlier in Phase 3).
    //   - Hash and packing must be consistent (we rely on parse_schema_json
    //     / SchemaRegistry to build them, so we only check non-empty here).
    if (rec.owner_uid.empty() || rec.schema_id.empty())
        return O::kForbiddenOwner;
    if (rec.packing.empty())
        return O::kForbiddenOwner;

    std::unique_lock lk(pImpl->mu);
    const SchemaKey  key{rec.owner_uid, rec.schema_id};

    auto it = pImpl->schemas.find(key);
    if (it != pImpl->schemas.end())
    {
        // Existing record under this (owner, id).  Idempotent only if
        // hash AND packing match exactly — bytewise-equal layout means
        // the caller is re-asserting the same record.
        if (it->second.hash == rec.hash && it->second.packing == rec.packing)
            return O::kIdempotent;
        return O::kHashMismatchSelf;
    }

    // New record.  Stamp the registration time and insert.
    rec.registered_at = std::chrono::system_clock::now();
    pImpl->schemas.emplace(key, std::move(rec));
    ++pImpl->counters.schema_registered_total;
    return O::kCreated;
}

std::size_t HubState::_on_schemas_evicted_for_owner(const std::string &owner_uid)
{
    if (owner_uid.empty()) return 0;

    // Hub-globals (owner=="hub") must never be evicted by this op — they
    // only leave HubState when the hub process exits.
    if (owner_uid == "hub") return 0;

    std::size_t        removed = 0;
    {
        std::unique_lock lk(pImpl->mu);
        for (auto it = pImpl->schemas.begin(); it != pImpl->schemas.end(); )
        {
            if (it->first.first == owner_uid)
            {
                it = pImpl->schemas.erase(it);
                ++removed;
            }
            else
            {
                ++it;
            }
        }
        pImpl->counters.schema_evicted_total += removed;
    }
    return removed;
}

schema::CitationOutcome HubState::_validate_schema_citation(
    const std::string &/*citer_uid*/,
    const std::string &channel_producer_uid,
    const std::string &cited_owner,
    const std::string &cited_id,
    const std::array<uint8_t, 32> &expected_hash,
    const std::string &expected_packing)
{
    using R = schema::CitationOutcome::Reason;
    schema::CitationOutcome out{R::kOk, {}};

    // Rule 1 — cited owner must be either "hub" or A registered
    // producer on the channel.  Cross-citation of a non-producer role
    // is rejected even on hash match (HEP-CORE-0034 §9.1, §9.3).
    // Cheap check, do it before taking lock.
    //
    // Multi-producer note: the parameter is named
    // `channel_producer_uid` (singular) because the broker calls this
    // once per admission with the admitted producer's uid (Wave M2.5
    // step 3 — `_on_producer_added`).  For Fan-In channels, each
    // producer's REG_REQ → CONSUMER citation runs through this check
    // independently with the admitting producer's uid; the signature
    // does not need to be widened because each call resolves a single
    // producer's claim against the channel-wide invariant.
    if (cited_owner != "hub" && cited_owner != channel_producer_uid)
    {
        out.reason = R::kCrossCitation;
        out.detail = "cited owner '" + cited_owner +
                     "' is neither 'hub' nor channel producer '" +
                     channel_producer_uid + "'";
    }
    else
    {
        // Rules 2 + 3 — owner-known + record-exists + fingerprint-match,
        // all under one reader lock.
        std::shared_lock lk(pImpl->mu);
        const bool owner_known =
            cited_owner == "hub" ||
            pImpl->roles.find(cited_owner) != pImpl->roles.end();

        if (!owner_known)
        {
            out.reason = R::kUnknownOwner;
            out.detail = "owner '" + cited_owner +
                         "' is not a registered role and is not 'hub'";
        }
        else
        {
            auto it = pImpl->schemas.find(SchemaKey{cited_owner, cited_id});
            if (it == pImpl->schemas.end())
            {
                out.reason = R::kUnknownSchema;
                out.detail =
                    "no record under (" + cited_owner + ", " + cited_id + ")";
            }
            else if (it->second.hash != expected_hash ||
                     it->second.packing != expected_packing)
            {
                out.reason = R::kFingerprintMismatch;
                out.detail = "record (" + cited_owner + ", " + cited_id +
                             ") exists but hash or packing differs";
            }
            // else: fingerprint matches → out.reason stays kOk.
        }
    }

    if (!out.ok())
    {
        std::unique_lock lk(pImpl->mu);
        ++pImpl->counters.schema_citation_rejected_total;
    }
    return out;
}

} // namespace pylabhub::hub
