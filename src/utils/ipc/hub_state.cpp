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

} // namespace pylabhub::hub
