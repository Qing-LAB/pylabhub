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
    bool fire = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->roles.find(uid);
        if (it == pImpl->roles.end()) return;
        if (it->second.state != RoleState::Disconnected)
        {
            it->second.state = RoleState::Disconnected;
            fire             = true;
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

void HubState::_set_shm_block(ShmBlockRef ref)
{
    std::unique_lock lk(pImpl->mu);
    pImpl->shm_blocks.insert_or_assign(ref.channel_name, std::move(ref));
}

void HubState::_bump_counter(const std::string &key, uint64_t n)
{
    std::unique_lock lk(pImpl->mu);
    pImpl->counters.msg_type_counts[key] += n;
}

void HubState::_set_role_state_metrics(const BrokerCounters &snapshot)
{
    std::unique_lock lk(pImpl->mu);
    pImpl->counters = snapshot;
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

    _bump_counter("REG_REQ");
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

    _bump_counter("CONSUMER_REG_REQ");
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
    _bump_counter("CONSUMER_DEREG_REQ");
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

    _bump_counter("HEARTBEAT_REQ");
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
    _set_channel_status(channel, ChannelStatus::PendingReady);
    if (!role_uid.empty()) _set_role_disconnected(role_uid);
    {
        std::unique_lock lk(pImpl->mu);
        ++pImpl->counters.ready_to_pending_total;
    }
}

void HubState::_on_pending_timeout(const std::string &channel)
{
    if (!is_valid_identifier(channel, IdentifierKind::Channel))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    _set_channel_status(channel, ChannelStatus::Closing);
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
    _bump_counter("METRICS_REPORT_REQ");
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

    _bump_counter("BAND_JOIN_REQ");
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
    _bump_counter("BAND_LEAVE_REQ");
}

void HubState::_on_peer_connected(PeerEntry peer)
{
    if (!is_valid_identifier(peer.uid, IdentifierKind::PeerUid))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    _set_peer_connected(std::move(peer));
    _bump_counter("HUB_PEER_HELLO");
}

void HubState::_on_peer_disconnected(const std::string &hub_uid)
{
    if (!is_valid_identifier(hub_uid, IdentifierKind::PeerUid))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    _set_peer_disconnected(hub_uid);
    _bump_counter("HUB_PEER_BYE");
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

} // namespace pylabhub::hub
