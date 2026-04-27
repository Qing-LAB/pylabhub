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

const char *to_string(ChannelStatus s) noexcept
{
    switch (s)
    {
    case ChannelStatus::PendingReady: return "PendingReady";
    case ChannelStatus::Ready:        return "Ready";
    case ChannelStatus::Closing:      return "Closing";
    }
    return "Unknown";
}

const char *to_string(RoleState s) noexcept
{
    switch (s)
    {
    case RoleState::Connected:    return "Connected";
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

HandlerId HubState::subscribe_channel_opened(ChannelOpenedHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->ch_opened, std::move(h));
}
HandlerId HubState::subscribe_channel_status_changed(ChannelStatusChangedHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->ch_status_changed, std::move(h));
}
HandlerId HubState::subscribe_channel_closed(ChannelClosedHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->ch_closed, std::move(h));
}
HandlerId HubState::subscribe_consumer_added(ConsumerAddedHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->cons_added, std::move(h));
}
HandlerId HubState::subscribe_consumer_removed(ConsumerRemovedHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->cons_removed, std::move(h));
}
HandlerId HubState::subscribe_role_registered(RoleRegisteredHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->role_reg, std::move(h));
}
HandlerId HubState::subscribe_role_disconnected(RoleDisconnectedHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->role_disc, std::move(h));
}
HandlerId HubState::subscribe_band_joined(BandJoinedHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->band_joined, std::move(h));
}
HandlerId HubState::subscribe_band_left(BandLeftHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->band_left, std::move(h));
}
HandlerId HubState::subscribe_peer_connected(PeerConnectedHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->peer_conn, std::move(h));
}
HandlerId HubState::subscribe_peer_disconnected(PeerDisconnectedHandler h)
{
    return add_handler(pImpl->handlers_mu, pImpl->next_handler_id,
                       pImpl->peer_disc, std::move(h));
}

void HubState::unsubscribe(HandlerId id) noexcept
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

void HubState::_set_channel_status(const std::string &name, ChannelStatus s)
{
    ChannelEntry fired;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(name);
        if (it == pImpl->channels.end()) return;
        it->second.status      = s;
        it->second.state_since = std::chrono::steady_clock::now();
        fired                  = it->second;
    }
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->ch_status_changed)) h(fired);
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

void HubState::_update_role_heartbeat(const std::string                     &uid,
                                      std::chrono::steady_clock::time_point  when)
{
    std::unique_lock lk(pImpl->mu);
    auto             it = pImpl->roles.find(uid);
    if (it == pImpl->roles.end()) return;
    it->second.last_heartbeat = when;
    if (it->second.state == RoleState::Disconnected)
        it->second.state = RoleState::Connected;
}

void HubState::_update_role_metrics(const std::string                     &uid,
                                    nlohmann::json                         metrics,
                                    std::chrono::system_clock::time_point  when)
{
    std::unique_lock lk(pImpl->mu);
    auto             it = pImpl->roles.find(uid);
    if (it == pImpl->roles.end()) return;
    it->second.latest_metrics       = std::move(metrics);
    it->second.metrics_collected_at = when;
}

void HubState::_set_role_disconnected(const std::string &uid)
{
    bool        fire           = false;
    std::size_t schemas_evicted = 0;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->roles.find(uid);
        if (it == pImpl->roles.end()) return;
        if (it->second.state != RoleState::Disconnected)
        {
            it->second.state = RoleState::Disconnected;
            fire             = true;

            // HEP-CORE-0034 §7.2 — schemas evict atomically with the
            // producer's role transition to Disconnected.  The schema's
            // lifetime is the role's process lifetime; once the role is
            // gone from the network, its private records have no
            // authority and must be removed before any later citer can
            // observe a stale entry.  Hub-globals (owner=="hub") are
            // never touched here.
            //
            // We do the eviction inside the same lock as the state
            // transition, so a snapshot taken after this op sees a
            // consistent (Disconnected role, no orphan schemas) view.
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

void HubState::_set_channel_closing_deadline(
    const std::string                    &name,
    std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lk(pImpl->mu);
    auto             it = pImpl->channels.find(name);
    if (it == pImpl->channels.end()) return;
    it->second.closing_deadline = deadline;
}

void HubState::_set_channel_zmq_node_endpoint(const std::string &name,
                                              std::string        endpoint)
{
    std::unique_lock lk(pImpl->mu);
    auto             it = pImpl->channels.find(name);
    if (it == pImpl->channels.end()) return;
    it->second.zmq_node_endpoint = std::move(endpoint);
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
template <typename Impl>
RoleEntry upsert_role_locked(Impl &impl, const std::string &uid,
                             const std::string &pubkey_z85,
                             const std::string &added_channel,
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
        r.uid            = uid;
        r.name           = derived_name;
        r.role_tag       = derived_tag;
        r.pubkey_z85     = pubkey_z85;
        r.state          = RoleState::Connected;
        r.last_heartbeat = heartbeat_when;
        if (!added_channel.empty()) r.channels.push_back(added_channel);
        auto [new_it, _] = impl.roles.emplace(uid, std::move(r));
        return new_it->second;
    }
    auto &ex = it->second;
    // name + role_tag are derived caches — re-derive on every upsert
    // so the invariant role_tag == extract_role_tag(uid) always holds.
    ex.name     = derived_name;
    ex.role_tag = derived_tag;
    if (!pubkey_z85.empty()) ex.pubkey_z85 = pubkey_z85;
    ex.state          = RoleState::Connected;
    ex.last_heartbeat = heartbeat_when;
    if (!added_channel.empty() &&
        std::find(ex.channels.begin(), ex.channels.end(), added_channel) ==
            ex.channels.end())
    {
        ex.channels.push_back(added_channel);
    }
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
    if (!entry.producer_role_uid.empty() &&
        !is_valid_identifier(entry.producer_role_uid, IdentifierKind::RoleUid))
    {
        bump_invalid_identifier(*pImpl);
        return;
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
    const std::string channel_name    = entry.name;
    const std::string producer_uid    = entry.producer_role_uid;
    const std::string producer_pubkey = entry.zmq_pubkey;
    const bool        has_shm         = entry.has_shared_memory;
    const std::string shm_name        = entry.shm_name;

    _set_channel_opened(std::move(entry));

    if (!producer_uid.empty())
    {
        RoleEntry fired = upsert_role_locked(
            *pImpl, producer_uid, producer_pubkey, channel_name,
            std::chrono::steady_clock::now());
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_reg))
            h(fired);
    }

    if (has_shm)
        _set_shm_block(ShmBlockRef{channel_name, shm_name});

}

void HubState::_on_channel_closed(const std::string &name, ChannelCloseReason why)
{
    if (!is_valid_identifier(name, IdentifierKind::Channel))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    _set_channel_closed(name);
    // Remove channel from each role's channels list (so a dropped channel
    // no longer appears under roles[*].channels).
    {
        std::unique_lock lk(pImpl->mu);
        for (auto &[uid, role] : pImpl->roles)
        {
            auto it = std::find(role.channels.begin(), role.channels.end(), name);
            if (it != role.channels.end()) role.channels.erase(it);
        }
        pImpl->shm_blocks.erase(name);
    }
    _bump_counter(std::string("close:") + to_string(why));
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
    // Remove this channel from the role's channels list; do NOT mark the
    // role disconnected — the role may still be producing on other
    // channels or reachable via bands / peer federation.
    if (!role_uid.empty())
    {
        std::unique_lock lk(pImpl->mu);
        auto it = pImpl->roles.find(role_uid);
        if (it != pImpl->roles.end())
        {
            auto &chs = it->second.channels;
            auto  rm  = std::find(chs.begin(), chs.end(), channel);
            if (rm != chs.end()) chs.erase(rm);
        }
    }
}

void HubState::_on_heartbeat(const std::string                   &channel,
                             const std::string                   &role_uid,
                             std::chrono::steady_clock::time_point when,
                             const std::optional<nlohmann::json> &metrics)
{
    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        (!role_uid.empty() && !is_valid_identifier(role_uid, IdentifierKind::RoleUid)))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    // 1. Channel liveness: first heartbeat transitions PendingReady -> Ready;
    //    subsequent heartbeats update last_heartbeat without a status change.
    //    Per HEP-CORE-0023 §2.5, every PendingReady -> Ready transition (first
    //    heartbeat OR recovery) increments `pending_to_ready_total`.
    bool fire_channel_ready = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel);
        if (it != pImpl->channels.end())
        {
            it->second.last_heartbeat = when;
            if (it->second.status == ChannelStatus::PendingReady)
            {
                it->second.status      = ChannelStatus::Ready;
                it->second.state_since = when;
                ++pImpl->counters.pending_to_ready_total;
                fire_channel_ready     = true;
            }
        }
    }
    if (fire_channel_ready)
    {
        ChannelEntry fired;
        {
            std::shared_lock rlk(pImpl->mu);
            auto             it = pImpl->channels.find(channel);
            if (it != pImpl->channels.end()) fired = it->second;
        }
        for (auto &h : snapshot_handlers(pImpl->handlers_mu,
                                         pImpl->ch_status_changed))
            h(fired);
    }

    // 2. Role liveness: advance last_heartbeat; if Disconnected, revive.
    if (!role_uid.empty())
        _update_role_heartbeat(role_uid, when);

    // 3. Role metrics: piggybacked in HEARTBEAT_REQ (HEP-0019 §1).
    if (metrics.has_value() && !role_uid.empty())
        _update_role_metrics(role_uid, *metrics,
                             std::chrono::system_clock::now());

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
    // HEP-CORE-0023 §2.1: Ready -> Pending demotion.  Role is NOT marked
    // Disconnected here — Pending means "suspicious, may recover via the
    // next heartbeat".  Role disconnect (if any) happens at Pending ->
    // deregistered (`_on_pending_timeout`).  `role_uid` is informational
    // only at this layer; the broker uses it for logging.
    //
    // Atomic transition + counter under a single writer-lock so the
    // counter only bumps when an actual Ready -> Pending demotion fires.
    bool         transitioned = false;
    ChannelEntry fired_entry;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel);
        if (it != pImpl->channels.end() &&
            it->second.status == ChannelStatus::Ready)
        {
            it->second.status      = ChannelStatus::PendingReady;
            it->second.state_since = std::chrono::steady_clock::now();
            ++pImpl->counters.ready_to_pending_total;
            fired_entry  = it->second;
            transitioned = true;
        }
    }
    if (transitioned)
    {
        for (auto &h : snapshot_handlers(pImpl->handlers_mu,
                                         pImpl->ch_status_changed))
            h(fired_entry);
    }
}

void HubState::_on_pending_timeout(const std::string &channel)
{
    if (!is_valid_identifier(channel, IdentifierKind::Channel))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    // HEP-CORE-0023 §2.1: Pending -> deregistered, **no grace, no Closing
    // intermediate**.  Closing is reserved for the voluntary-close path
    // (DEREG_REQ / admin / script close).  Eligibility check + close +
    // counter:
    //   - eligibility: channel exists and is in PendingReady;
    //   - close: same as `_on_channel_closed(HeartbeatTimeout)`
    //     (erases entry, removes from each role's `channels` list, fires
    //     close handlers, bumps the per-reason msg-type counter);
    //   - counter: bump `pending_to_deregistered_total` only when an
    //     actual Pending -> deregistered transition fired (per §2.5).
    {
        std::shared_lock rlk(pImpl->mu);
        auto             it = pImpl->channels.find(channel);
        if (it == pImpl->channels.end() ||
            it->second.status != ChannelStatus::PendingReady)
            return;
    }
    _on_channel_closed(channel, ChannelCloseReason::HeartbeatTimeout);
    {
        std::unique_lock lk(pImpl->mu);
        ++pImpl->counters.pending_to_deregistered_total;
    }
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
    // path. No liveness effect: a role could be stalled on the heartbeat
    // axis but still drip metrics on this channel. `channel` is part of
    // the wire schema; at the HubState level it's informational for the
    // counter key only (metrics collapse onto RoleEntry per HEP §8).
    if (!role_uid.empty())
        _update_role_metrics(role_uid, std::move(metrics), when);
    (void)channel; // reserved for future per-channel metrics splitting (G2.2.4)
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

    // Rule 1 — cited owner must be either "hub" or the channel's producer.
    // Cross-citation of a third role is rejected even on hash match
    // (HEP-CORE-0034 §9.1, §9.3).  Cheap check, do it before taking lock.
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
