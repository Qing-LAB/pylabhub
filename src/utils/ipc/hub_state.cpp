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

// ChannelTopology policy surface — grouped under `topology` namespace
// per finding #23.  Canonical wire strings per HEP-CORE-0007 §12.3;
// decision matrix per HEP-CORE-0017 §3.3.0; overwrite + cardinality
// semantics per tech draft §5.1.
namespace topology
{

const char *to_string(ChannelTopology t) noexcept
{
    switch (t)
    {
    case ChannelTopology::FanIn:    return "fan-in";
    case ChannelTopology::FanOut:   return "fan-out";
    case ChannelTopology::OneToOne: return "one-to-one";
    }
    return "unknown";
}

std::optional<ChannelTopology> parse(std::string_view s) noexcept
{
    if (s == "fan-in")     return ChannelTopology::FanIn;
    if (s == "fan-out")    return ChannelTopology::FanOut;
    if (s == "one-to-one") return ChannelTopology::OneToOne;
    return std::nullopt;
}

bool transport_compatible(ChannelTopology t,
                          std::string_view data_transport) noexcept
{
    if (t == ChannelTopology::FanIn && data_transport == "shm")
        return false;
    if (data_transport != "zmq" && data_transport != "shm")
        return false;
    return true;
}

const char *check_cardinality(ChannelTopology t,
                              AdmissionSide   side,
                              std::size_t     existing_producers,
                              std::size_t     existing_consumers) noexcept
{
    const bool is_consumer = (side == AdmissionSide::Consumer);
    switch (t)
    {
    case ChannelTopology::FanIn:
        if (is_consumer && existing_consumers >= 1)
            return "FAN_IN_IS_SINGLE_CONSUMER";
        return nullptr;
    case ChannelTopology::FanOut:
        if (!is_consumer && existing_producers >= 1)
            return "FAN_OUT_IS_SINGLE_PRODUCER";
        return nullptr;
    case ChannelTopology::OneToOne:
        if (is_consumer && existing_consumers >= 1)
            return "ONE_TO_ONE_CARDINALITY_VIOLATED";
        if (!is_consumer && existing_producers >= 1)
            return "ONE_TO_ONE_CARDINALITY_VIOLATED";
        return nullptr;
    }
    return "INVALID_REQUEST";
}

} // namespace topology

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
    /// HEP-CORE-0036 §4.1 — per-channel access scaffolding (allowlist
    /// + SHM secret).  Broker-internal; NOT exposed through
    /// `HubStateSnapshot` because callers that need it (broker handlers
    /// for REG / CONSUMER_REG / CHANNEL_AUTH_UPDATE emission) all run
    /// in the broker process and use the targeted `channel_access`
    /// read accessor.
    std::unordered_map<std::string, ChannelAccessEntry> channel_access_index;

    /// HEP-CORE-0042 §5.2 — `instance[P]`.  Monotonic uint64_t per
    /// producer role_uid, incremented on every registration
    /// (`_on_producer_added`).  Broker returns the current value in
    /// PRODUCER_REG_ACK and rejects any CHANNEL_AUTH_APPLIED_REQ that
    /// echoes a stale instance_id, closing the crashed-producer
    /// stale-APPLIED_REQ race described in HEP-CORE-0042 §5.2.  Read
    /// by `handle_channel_auth_applied_req` (broker single-threaded
    /// ROUTER dispatch — no external synchronization needed beyond
    /// the existing `mu`).
    std::unordered_map<std::string, std::uint64_t> producer_instance;

    // I-REPLAY-BOUND (HEP-CORE-0046 §8.1).  Per-role
    // sliding-window dedup for REG-family client_nonces.  Entries older
    // than `nonce_seen()`'s `window_ms` argument are pruned on the next
    // `nonce_seen()` call for the same role_uid.  Prune-on-access is
    // sufficient because the dispatch thread serializes access under
    // I-ROUTER-SERIAL; a background sweep would only matter if a role's
    // uid stopped registering while others kept churning, which is
    // bounded by the sizeof(client_nonce)==16-bytes footprint.
    struct NonceEntry
    {
        std::string   nonce;
        std::uint64_t wall_ts;
    };
    std::unordered_map<std::string, std::vector<NonceEntry>> nonce_dedup;

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

    // ── HEP-CORE-0039 §3.1 capture metadata ────────────────────────────
    // `hub_uid` is set once via `HubState::set_hub_uid` during
    // HubHost initialization and read on every snapshot under
    // `mu`.  `snapshot_seq_counter_` is incremented atomically on
    // every snapshot — the first live snapshot has seq == 1 (see
    // HEP-0039 §8.3 + §3.1 metadata table).
    std::string                                    hub_uid;
    std::atomic<std::uint64_t>                     snapshot_seq_counter_{0};
};

HubState::HubState() : pImpl(std::make_unique<Impl>()) {}
HubState::~HubState() = default;

// ─── Read-only accessors ────────────────────────────────────────────────────

void HubState::set_hub_uid(std::string hub_uid)
{
    std::unique_lock lk(pImpl->mu);
    pImpl->hub_uid = std::move(hub_uid);
}

HubStateSnapshot HubState::snapshot() const
{
    std::shared_lock lk(pImpl->mu);
    HubStateSnapshot s;
    s.channels      = pImpl->channels;
    s.roles         = pImpl->roles;
    s.bands         = pImpl->bands;
    s.peers         = pImpl->peers;
    s.shm_blocks    = pImpl->shm_blocks;
    s.schemas       = pImpl->schemas;
    s.counters      = pImpl->counters;
    s.captured_at   = std::chrono::system_clock::now();
    s.captured_mono = std::chrono::steady_clock::now();
    s.hub_uid       = pImpl->hub_uid;
    // fetch_add returns the value BEFORE the add, so +1 gives 1 on
    // first call — matches HEP-0039 §3.1 "first live snapshot has
    // seq == 1; seq == 0 is reserved for default-constructed".
    s.snapshot_seq  =
        pImpl->snapshot_seq_counter_.fetch_add(
            1, std::memory_order_relaxed) +
        1;
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

std::optional<ChannelAccessEntry>
HubState::channel_access(const std::string &channel_name) const
{
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->channel_access_index.find(channel_name);
    if (it == pImpl->channel_access_index.end()) return std::nullopt;
    return it->second;
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

nlohmann::json HubState::channel_metrics_snapshot(const std::string &channel) const
{
    // Wave M1.4 (2026-05-11) — replaces `BrokerServiceImpl::metrics_store_`
    // as the source of channel metrics.  Reads per-presence rows
    // directly from RoleEntry.presences[].latest_metrics, written by
    // `_on_heartbeat` (HEP-CORE-0019 §2.3 Phase 6 "metrics piggyback on
    // heartbeat").  Iterates the channel's `producers[]` + `consumers[]`
    // to bound the scan; for each, looks up the owning role and reads
    // the matching presence row.  O(producer_count + consumer_count).
    nlohmann::json result    = nlohmann::json::object();
    nlohmann::json producers = nlohmann::json::object();
    nlohmann::json consumers = nlohmann::json::object();
    {
        std::shared_lock lk(pImpl->mu);
        auto             cit = pImpl->channels.find(channel);
        if (cit == pImpl->channels.end()) return result;

        for (const auto &prod : cit->second.producers)
        {
            if (prod.role_uid.empty()) continue;
            auto rit = pImpl->roles.find(prod.role_uid);
            if (rit == pImpl->roles.end()) continue;
            const auto *p = rit->second.find_presence(channel, "producer");
            if (p == nullptr || p->latest_metrics.is_null()) continue;
            nlohmann::json one = p->latest_metrics;
            // pid is a per-channel-producer property, lives on
            // ChannelEntry.producers[].producer_pid (not on RolePresence).
            one["pid"]               = prod.producer_pid;
            producers[prod.role_uid] = std::move(one);
        }
        for (const auto &cons : cit->second.consumers)
        {
            if (cons.role_uid.empty()) continue;
            auto rit = pImpl->roles.find(cons.role_uid);
            if (rit == pImpl->roles.end()) continue;
            const auto *p = rit->second.find_presence(channel, "consumer");
            if (p == nullptr || p->latest_metrics.is_null()) continue;
            consumers[cons.role_uid] = p->latest_metrics;
        }
    }
    if (!producers.empty()) result["producers"] = std::move(producers);
    if (!consumers.empty()) result["consumers"] = std::move(consumers);
    return result;
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

// ─── Internal helpers used by terminal-cleanup paths ───────────────────────

namespace
{

/// Wave M3 step 5f+i (2026-05-11) — terminal-cleanup cascade shared by
/// `_set_role_disconnected` (unconditional force-erase) and
/// `_dispatch_role_disconnected_if_dead` (predicate-guarded production
/// trigger).  Caller holds the writer lock on `impl.mu`.  Performs:
///   1. Schema cascade (HEP-CORE-0034 §7.2): evict every owner-namespaced
///      schema record where `owner_uid == uid`.  Hub-globals immune.
///   2. Band cascade (HEP-CORE-0030 §8): remove `uid` from every
///      `BandEntry.members`; auto-delete empty bands.
///
/// Populates `bands_left_out` with the band names from which `uid` was
/// actually removed (caller fires `band_left` handler per entry with
/// reason="role_closed").  Returns schemas evicted count.
template <typename Impl>
inline std::size_t cascade_role_terminal_cleanup_locked(
    Impl &impl,
    const std::string &uid,
    std::vector<std::string> &bands_left_out)
{
    // Schema cascade.
    std::size_t schemas_evicted = 0;
    for (auto sit = impl.schemas.begin(); sit != impl.schemas.end(); )
    {
        if (sit->first.first == uid)
        {
            sit = impl.schemas.erase(sit);
            ++schemas_evicted;
        }
        else
        {
            ++sit;
        }
    }

    // Band cascade.  Auto-delete empty bands matches `_set_band_left`'s
    // behaviour (the band's last member leaving deletes the band).
    bands_left_out.reserve(impl.bands.size());
    const auto now = std::chrono::steady_clock::now();
    for (auto bit = impl.bands.begin(); bit != impl.bands.end(); )
    {
        auto &m   = bit->second.members;
        auto  old = m.size();
        m.erase(std::remove_if(m.begin(), m.end(),
                               [&](const BandMember &x) { return x.role_uid == uid; }),
                m.end());
        if (m.size() < old)
        {
            bands_left_out.push_back(bit->first);
            bit->second.last_activity = now;
        }
        if (m.empty()) bit = impl.bands.erase(bit);
        else           ++bit;
    }

    return schemas_evicted;
}

}  // namespace

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

// Internal: atomic consumer append with topology + cardinality
// enforcement.  The re-registration case (same role_uid or same pid
// already present) is idempotent — the existing row is replaced and
// no cardinality check fires.  A NEW consumer is subject to
// topology-mismatch + cardinality gates.
//
// Returns ConsumerAdmissionResult so `_on_consumer_joined` can decide
// whether to fire downstream handlers (role registration, etc.).
ConsumerAdmissionResult
HubState::_add_consumer(const std::string&                        channel,
                        ConsumerEntry                             entry,
                        std::optional<ChannelTopology>            declared_topology,
                        std::optional<ChannelSchemaInvariants>    open_schema,
                        std::optional<ChannelTransportInvariants> open_transport)
{
    ConsumerAdmissionResult result;
    ConsumerEntry           fired;
    ChannelEntry            fired_channel;
    bool                    did_open_channel = false;

    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel);

        if (it == pImpl->channels.end())
        {
            // Channel doesn't exist.  Under HEP-CORE-0017 §3.3.0 the
            // consumer opens the channel iff (a) the declared topology
            // makes the consumer the binding side (only fan-in
            // qualifies) AND (b) the caller supplied open invariants.
            // Otherwise silent-skip (matches pre-topology behavior:
            // "the wire hasn't reached a state where this consumer can
            // be admitted").
            const bool consumer_is_opener =
                declared_topology.has_value() &&
                *declared_topology == ChannelTopology::FanIn &&
                open_schema.has_value() &&
                open_transport.has_value();
            if (!consumer_is_opener) return result;

            const char* open_err = nullptr;
            ChannelEntry* new_entry = _open_channel_locked(
                channel, *open_schema, *open_transport,
                ChannelTopology::FanIn, open_err);
            if (!new_entry)
            {
                result.topology_error_code = open_err;
                return result;
            }
            new_entry->consumers.push_back(std::move(entry));
            fired            = new_entry->consumers.back();
            fired_channel    = *new_entry;
            did_open_channel = true;
            result.admitted        = true;
            result.channel_opened  = true;
            result.invariant_result = InvariantSetResult::Created;
        }
        else
        {
            auto &channel_entry = it->second;

            // Topology declaration check.  Empty inherits stored;
            // explicit non-matching → TOPOLOGY_MISMATCH.
            if (declared_topology && *declared_topology != channel_entry.topology)
            {
                result.topology_error_code = "TOPOLOGY_MISMATCH";
                return result;
            }

            auto &cons = channel_entry.consumers;

            // Detect re-registration (same role_uid already present)
            // — idempotent, skip cardinality check.  Role identity is
            // uid-only per HEP-CORE-0033 §G2.2.0b + HEP-CORE-0023 §2.1;
            // pid is metadata, not identity, so a pid collision does
            // NOT qualify as re-registration.
            auto is_dupe = [&](const ConsumerEntry &c) noexcept {
                return !entry.role_uid.empty() && c.role_uid == entry.role_uid;
            };
            const bool is_reregistration =
                std::any_of(cons.begin(), cons.end(), is_dupe);

            if (!is_reregistration)
            {
                if (const char *err = topology::check_cardinality(
                        channel_entry.topology, topology::AdmissionSide::Consumer,
                        channel_entry.producers.size(), cons.size()))
                {
                    result.topology_error_code = err;
                    return result;
                }
            }

            cons.erase(std::remove_if(cons.begin(), cons.end(), is_dupe),
                       cons.end());
            cons.push_back(std::move(entry));
            fired                   = cons.back();
            result.admitted         = true;
            result.invariant_result = InvariantSetResult::IdempotentEqual;
        }
    }
    if (!result.admitted) return result;

    // Fire handlers post-lock.  ch_opened first (mirrors
    // _on_producer_added's ordering) so subscribers see the channel
    // before they see its first consumer.
    if (did_open_channel)
    {
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->ch_opened))
            h(fired_channel);
    }
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->cons_added))
        h(channel, fired);
    return result;
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
        if (removed)
        {
            // Symmetric with `_on_producer_dropped` / `_on_pending_timeout`
            // (HEP-CORE-0042 §5.4 reset triggers) — clear this role's
            // confirmed_version in the ledger so a stale entry cannot
            // service a future `is_visible_to(role_uid, ...)` query if
            // the role_uid were re-used (UID-conflict policy blocks
            // re-use at RoleEntry level, so this is memory hygiene, but
            // the asymmetry with the producer path was a review finding
            // — Agent-4, 2026-07-13).  Whole-channel teardown
            // (`_on_channel_access_closed`) erases the entire
            // ChannelAccessEntry so the ledger goes with it; this path
            // handles the non-last-consumer case where the channel
            // survives.
            auto acc_it = pImpl->channel_access_index.find(channel);
            if (acc_it != pImpl->channel_access_index.end())
            {
                acc_it->second.ledger.reset_role_confirmation(role_uid);
            }
        }
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
    // Wave M3 step 4 (2026-05-11): TERMINAL CLEANUP — unconditional
    // force-erase entry point.  Used by L2 tests + admin/script
    // force-disconnect.  Production code uses
    // `_dispatch_role_disconnected_if_dead` which is predicate-guarded.
    //
    // Cleanup cascades (under one writer lock):
    //   1. Schema cascade (HEP-CORE-0034 §7.2 owner-lifetime).
    //   2. Band cascade (Wave M3 step 5i, HEP-CORE-0030 §8): remove
    //      this role from every band's member list; auto-delete empty
    //      bands.  Replaces the broker's old imperative
    //      `band_on_role_closed` trigger from channel-close fanout,
    //      which fired too eagerly (a multi-presence role would be
    //      evicted from bands when ONE of its channels closed, even
    //      if the role itself was alive on another channel).  Now
    //      band membership tracks role lifetime, not channel lifetime.
    //   3. Erase role entry.
    //
    // Handlers fire AFTER lock release.  Order: `role_disc(uid)` first,
    // then `band_left(band, uid)` per affected band.  Subscribers
    // (script runner; broker BAND_LEAVE_NOTIFY) observe state with the
    // role already gone.
    bool                      fire           = false;
    std::size_t               schemas_evicted = 0;
    std::vector<std::string>  bands_left;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->roles.find(uid);
        if (it == pImpl->roles.end()) return;  // idempotent — already gone
        fire = true;
        schemas_evicted = cascade_role_terminal_cleanup_locked(*pImpl, uid, bands_left);
        pImpl->counters.schema_evicted_total += schemas_evicted;
        pImpl->roles.erase(it);
    }
    if (!fire) return;
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_disc)) h(uid);
    for (const auto &band_name : bands_left)
    {
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->band_left))
            h(band_name, uid, "role_closed");
    }
}

void HubState::_dispatch_role_disconnected_if_dead(const std::string &uid)
{
    // Wave M3 step 5b (2026-05-11): production trigger for terminal
    // cleanup.  Atomic check-and-erase under the writer lock; a
    // concurrent REG_REQ that re-arms a presence between the caller's
    // transition and this call (TOCTOU window) is observed and the
    // cleanup becomes a no-op.  Idempotent.
    //
    // Shares the cascade body with `_set_role_disconnected` via
    // `cascade_role_terminal_cleanup_locked` — single recipe for
    // schema + band cleanup.  Step 5i (2026-05-11) added the band
    // cascade so role-lifetime band membership tracking works for
    // every terminal-cleanup path, not just channel-close.
    bool                      fire           = false;
    std::size_t               schemas_evicted = 0;
    std::vector<std::string>  bands_left;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->roles.find(uid);
        if (it == pImpl->roles.end()) return;       // already cleaned up
        if (it->second.any_presence_alive()) return; // re-armed; skip
        fire = true;
        schemas_evicted = cascade_role_terminal_cleanup_locked(*pImpl, uid, bands_left);
        pImpl->counters.schema_evicted_total += schemas_evicted;
        pImpl->roles.erase(it);
    }
    if (!fire) return;
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_disc)) h(uid);
    for (const auto &band_name : bands_left)
    {
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->band_left))
            h(band_name, uid, "role_closed");
    }
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

void HubState::_set_band_left(const std::string &band, const std::string &role_uid,
                                const std::string &reason)
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
    for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->band_left))
        h(band, role_uid, reason);
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

bool HubState::_set_channel_data_endpoint(const std::string &channel_name,
                                           std::string        endpoint)
{
    // Channel-scope endpoint update (HEP-CORE-0017 §3.3.0 binding-side
    // ownership).  Identifier validation is the caller's responsibility.
    if (channel_name.empty() || endpoint.empty())
        return false;
    std::unique_lock lk(pImpl->mu);
    auto             it = pImpl->channels.find(channel_name);
    if (it == pImpl->channels.end()) return false;
    it->second.set_data_endpoint(std::move(endpoint));
    return true;
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
/// and `role.short_tag` from the uid via `parse_role_uid()`.
///
/// Callers must hold no lock on entry; this helper takes the state
/// writer lock briefly and returns the post-mutation entry for event
/// dispatch.
/// Insert-or-update a `RolePresence` row on @p role.  Idempotent: if a
/// LIVE `(channel, role_type)` row already exists, it is left untouched
/// (re-REG of the same channel from the same role).  Post-H18 tombstone
/// removal (Wave-M3 step 5h, 2026-05-11), `on_dereg` and
/// `on_pending_timeout` ERASE the row instead of marking it
/// Disconnected — so the early-return path here only handles a live
/// presence's idempotent re-REG.  A Disconnected presence in
/// `presences[]` is now structurally impossible; re-REG after a prior
/// dereg lands on the create-new path (presence absent → push fresh
/// Connected/registering row).  Pure helper — caller must hold the
/// impl mutex.
///
/// Assertion is deliberately omitted here: the only way to reach the
/// early-return is `find_presence(...) != nullptr`, and every site
/// that erases a presence (`on_dereg`, `on_pending_timeout`) is the
/// inverse of this push.  If somehow a Disconnected row leaks into
/// `presences[]` in the future, audit those erasure sites first.
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
        r.short_tag   = derived_tag;
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
    // name + short_tag are derived caches — re-derive on every upsert
    // so the invariant short_tag == extract_short_tag(uid) always holds.
    ex.name     = derived_name;
    ex.short_tag = derived_tag;
    if (!pubkey_z85.empty()) ex.pubkey_z85 = pubkey_z85;
    if (!added_channel.empty() &&
        std::find(ex.channels.begin(), ex.channels.end(), added_channel) ==
            ex.channels.end())
    {
        ex.channels.push_back(added_channel);
    }
    upsert_presence_row_locked(ex, added_channel, role_type, heartbeat_when);
    // `disconnected_fired` reset retired by Wave M3 step 4
    // (commit forthcoming).  Terminal cleanup of `_set_role_disconnected`
    // means a "revival" path through this function would only fire
    // if `it != impl.roles.end()` — which itself requires that the
    // role had NOT been fully disconnected (any_presence_alive was
    // still true).  So no memoization reset is needed; the field is
    // gone.
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
    // Wave M2.5 step 6.5: pubkey lives on ProducerEntry, not channel-
    // scope.  Read from the first producer (test-only legacy path
    // accepts one producer per call).
    const std::string producer_pubkey = entry.producers.empty()
                                           ? std::string{}
                                           : entry.producers.front().zmq_pubkey;
    // HEP-CORE-0036 §5b.4: data_transport == "shm" classifies SHM; the
    // SHM block path is the channel name.
    const bool        has_shm         = (entry.data_transport == "shm");

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
        _set_shm_block(ShmBlockRef{channel_name, channel_name});

}

// Shared channel-open primitive per HEP-CORE-0017 §3.3.0.  Called
// from both `_on_producer_added` (fan-out / one-to-one — producer
// binds and opens) and `_on_consumer_joined` (fan-in — consumer
// binds and opens).  The framework-level invariant: neither role
// type "owns" channel creation; the topology decides which side is
// the binding-side opener.
//
// Caller MUST hold pImpl->mu writer lock.  Caller MUST fire the
// ch_opened handler chain AFTER releasing the lock.  Handler firing
// is left to callers because their handler ordering also involves
// role-registration + shm-block cascades that differ between the
// producer-side and consumer-side admission paths.
ChannelEntry*
HubState::_open_channel_locked(
    const std::string&                 channel_name,
    const ChannelSchemaInvariants&     schema,
    const ChannelTransportInvariants&  transport,
    ChannelTopology                    topology,
    const char*&                       topology_error_code)
{
    topology_error_code = nullptr;

    if (!topology::transport_compatible(topology, transport.data_transport))
    {
        topology_error_code = "TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT";
        return nullptr;
    }

    ChannelEntry entry;
    entry.name           = channel_name;
    entry.schema_hash    = schema.schema_hash;
    entry.schema_version = schema.schema_version;
    entry.schema_id      = schema.schema_id;
    entry.schema_blds    = schema.schema_blds;
    entry.schema_owner   = schema.schema_owner;
    entry.data_transport = transport.data_transport;
    entry.topology       = topology;

    auto [it, inserted] = pImpl->channels.insert_or_assign(
        channel_name, std::move(entry));
    (void)inserted;
    return &it->second;
}

// Atomic producer-side admission.  All checks (schema, transport,
// topology, cardinality, uid conflict) happen under the writer lock
// held for the mutation — no snapshot-then-mutate window.
ProducerAdmissionResult
HubState::_on_producer_added(const std::string&              channel_name,
                              ChannelSchemaInvariants         schema,
                              ChannelTransportInvariants      transport,
                              std::optional<ChannelTopology>  declared_topology,
                              ProducerEntry                   producer)
{
    ProducerAdmissionResult result;

    // Identifier validation at the op-entry boundary (mirrors
    // `_on_channel_registered`).  Invalid identifiers bump
    // sys.invalid_identifier_rejected and silent-drop; the wire layer
    // returns a typed error.
    // Grammar failures — defensive branch.  Production callers are
    // guarded by `BrokerServiceImpl::validate_identity_fields`, which
    // rejects with `INVALID_REQUEST` before reaching here.  Test-access
    // callers may bypass; report the failure explicitly so the broker
    // adapter maps to `INVALID_REQUEST` instead of misclassifying as
    // SCHEMA_MISMATCH / TRANSPORT_MISMATCH (retired 2026-07-09 per
    // review Bug #3).
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel) ||
        (!producer.role_uid.empty() &&
         !is_valid_identifier(producer.role_uid, IdentifierKind::RoleUid)) ||
        (!schema.schema_id.empty() &&
         !is_valid_identifier(schema.schema_id, IdentifierKind::Schema)))
    {
        bump_invalid_identifier(*pImpl);
        result.invalid_identifier = true;
        return result;
    }

    // Capture pieces needed for role/shm side-effects before taking the
    // writer lock (so we don't hold the writer lock across handler
    // dispatch).
    const std::string producer_uid     = producer.role_uid;
    const std::string producer_pubkey  = producer.zmq_pubkey;
    // HEP-CORE-0036 §5b.4: data_transport == "shm" is the canonical
    // SHM classifier; the SHM block name is the channel name.
    const bool        has_shm          = (transport.data_transport == "shm");

    ChannelEntry fired_entry;
    bool         did_fire_open = false;

    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel_name);

        if (it == pImpl->channels.end())
        {
            // Fresh channel — resolve topology (declared or default
            // OneToOne per HEP-CORE-0017 §3.3.0) and open via the
            // shared primitive.  Cardinality is trivially OK on a
            // fresh channel (0 producers, 0 consumers).
            const ChannelTopology effective_topology =
                declared_topology.value_or(ChannelTopology::OneToOne);
            const char* open_err = nullptr;
            ChannelEntry* new_entry = _open_channel_locked(
                channel_name, schema, transport, effective_topology,
                open_err);
            if (!new_entry)
            {
                result.topology_error_code = open_err;
                return result;
            }
            result.producer_result = new_entry->add_producer(std::move(producer));
            if (result.producer_result != AddProducerResult::Created)
            {
                // Roll back the channel-open: the entry was inserted
                // but adding the producer failed (uid conflict on a
                // fresh entry can't actually happen — the entry has
                // an empty producers[] — but preserve the invariant
                // "no ChannelEntry survives without at least one
                // producer or consumer" defensively).
                pImpl->channels.erase(channel_name);
                return result;
            }
            fired_entry             = *new_entry;
            did_fire_open           = true;
            result.invariant_result = InvariantSetResult::Created;
            result.channel_opened   = true;
        }
        else
        {
            // Existing channel.  Validate invariants first, then
            // topology declaration + cardinality.  Return on first
            // failure — order matches broker's error-code priority
            // (SCHEMA_MISMATCH / TRANSPORT_MISMATCH before topology
            // gate; topology-mismatch before cardinality).
            const auto &cur = it->second;
            auto reject_invariant = [&](const char *name) {
                result.invariant_result     = InvariantSetResult::RejectedMismatch;
                result.mismatched_invariant = name;
            };
            if (cur.schema_hash    != schema.schema_hash)    { reject_invariant("schema_hash");    return result; }
            if (cur.schema_version != schema.schema_version) { reject_invariant("schema_version"); return result; }
            if (cur.schema_id      != schema.schema_id)      { reject_invariant("schema_id");      return result; }
            if (cur.schema_blds    != schema.schema_blds)    { reject_invariant("schema_blds");    return result; }
            if (cur.schema_owner   != schema.schema_owner)   { reject_invariant("schema_owner");   return result; }
            if (cur.data_transport != transport.data_transport)
                                                              { reject_invariant("data_transport"); return result; }

            // Topology declaration check.  Empty inherits stored;
            // explicit non-matching → TOPOLOGY_MISMATCH.  Immutable
            // at creation, no promotion path.
            if (declared_topology && *declared_topology != cur.topology)
            {
                result.topology_error_code = "TOPOLOGY_MISMATCH";
                return result;
            }
            // Same-uid re-register detection.  When the incoming
            // producer's role_uid matches an existing one on this
            // channel, `add_producer` will return `RejectedUidConflict`
            // (broker surfaces as `UID_CONFLICT`) — the semantically
            // distinct "you already registered" error.  The cardinality
            // gate is skipped in this branch so we don't mask
            // UID_CONFLICT behind a topology-cardinality error code.
            const bool same_uid_reregister =
                !producer.role_uid.empty() &&
                cur.find_producer(producer.role_uid) != nullptr;

            if (!same_uid_reregister)
            {
                // Cardinality gate under stored topology.  Reflects live
                // counts because we hold the writer lock — no TOCTOU.
                if (const char *err = topology::check_cardinality(
                        cur.topology, topology::AdmissionSide::Producer,
                        cur.producers.size(), cur.consumers.size()))
                {
                    result.topology_error_code = err;
                    return result;
                }
            }

            result.invariant_result = InvariantSetResult::IdempotentEqual;
            result.producer_result  = it->second.add_producer(std::move(producer));
            if (result.producer_result != AddProducerResult::Created)
                return result;
            result.channel_opened = false;
        }

        // HEP-CORE-0042 §5.2 — bump producer_instance[P] on every
        // successful add.  First registration lifts the counter from
        // its default 0 → 1; re-registration under the same role_uid
        // (crash-restart) bumps to N+1.  Both cases invalidate any
        // in-flight CHANNEL_AUTH_APPLIED_REQ from a stale instance —
        // handle_channel_auth_applied_req compares the echoed
        // instance_id against the current counter and silently drops
        // mismatches (§5.2 stale-instance guard, P4 preservation).
        //
        // §5.4 mandates confirmed_version[K][P] = 0 on re-registration.
        // Erase the map entry (equivalent to reset-to-0 given
        // handle_consumer_attach_req_zmq treats a missing key as 0)
        // atomically with the counter bump.  Without this, a fresh
        // producer instance inherits the OLD instance's confirmed
        // state, making the fast-path admit a consumer whose pubkey
        // is not in the new instance's empty cache — CURVE handshake
        // fails (P4 direction A survives), but the P1 "one wait-path
        // cycle per rare event" promise is broken (persistent hard
        // failure until the next channel_version bump).
        //
        // Held under the same writer lock as the producer append so
        // the state triple stays consistent for any reader.
        if (!producer_uid.empty())
        {
            ++pImpl->producer_instance[producer_uid];
            auto acc_it = pImpl->channel_access_index.find(channel_name);
            if (acc_it != pImpl->channel_access_index.end())
            {
                acc_it->second.ledger
                    .reset_role_confirmation(producer_uid);
            }
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
        _set_shm_block(ShmBlockRef{channel_name, channel_name});

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
    bool transitioned     = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel_name);
        if (it == pImpl->channels.end()) return result;  // removed=false

        // Probe before mutating: is the uid registered, and is it the
        // LAST producer on this channel?
        //
        //  - Non-last (>=2 producers remain): remove now; channel
        //    survives.  Wave M3 step 5c (2026-05-11) — also transition
        //    the role's producer-presence Disconnected via on_dereg,
        //    maintain the `channels` cache invariant, and dispatch
        //    terminal cleanup after lock release.  Schema eviction
        //    is owner-lifetime (HEP-CORE-0034 §7.2) — handled by
        //    dispatch iff the role has no other alive presence.
        //  - Last (this drop empties the channel): leave the producer
        //    in the list and call `_on_channel_closed` below.
        if (it->second.find_producer(role_uid) == nullptr) return result;
        is_last_producer = (it->second.producer_count() == 1);
        if (!is_last_producer)
        {
            auto rm = it->second.remove_producer(role_uid);
            result.removed             = rm.removed;
            result.channel_now_empty   = false;  // by construction
            auto rit = pImpl->roles.find(role_uid);
            if (rit != pImpl->roles.end())
            {
                (void)rit->second.on_dereg(channel_name, "producer");
                (void)rit->second.drop_channel_if_orphaned(channel_name);
                transitioned = true;
            }
            // HEP-CORE-0042 §5.4 mandates confirmed_version[K][P] = 0
            // on producer disconnect.  Held under the same writer lock
            // as the producer removal for atomicity with the state
            // triple.  Erase (equivalent to reset-to-0) so subsequent
            // fast-path checks for this (K, P) treat the pair as
            // never-confirmed until the new instance's APPLIED_REQ
            // pipeline runs.  Last-producer path skips this — the
            // whole ChannelAccessEntry is nuked by
            // _on_channel_access_closed downstream.
            auto acc_it = pImpl->channel_access_index.find(channel_name);
            if (acc_it != pImpl->channel_access_index.end())
            {
                acc_it->second.ledger.reset_role_confirmation(role_uid);
            }
            // Fall through to dispatch after lock release.
        }
        // Last-producer path: leave the producer in producers[] so
        // _on_channel_closed can iterate it for atomic teardown.
    }

    if (!is_last_producer)
    {
        // Wave M3 step 5c: dispatch terminal cleanup iff the producer's
        // role is now fully Disconnected.  Cheap no-op otherwise.
        if (transitioned) _dispatch_role_disconnected_if_dead(role_uid);
        return result;
    }

    // Last-producer path: atomic teardown per HEP-CORE-0023 §2.1.1.
    // `_on_channel_closed` re-takes the writer lock; transitions every
    // producer + consumer presence on the channel; dispatches role
    // cleanup for each (schema cascade fires from dispatch per
    // HEP-CORE-0034 §7.2 owner-lifetime).
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

    // HEP-CORE-0023 §2.1.1 atomic teardown — collect every uid (producer
    // AND consumer) currently associated with this channel BEFORE
    // teardown.  All presences on the channel transition Disconnected
    // here: when the channel is gone, every party on it is by
    // definition terminated.  Producers transition first because they
    // own the channel; consumers transition for the same atomic-
    // teardown reason (HEP-CORE-0023 §2.1.1 — "all presences on a
    // closed channel are terminal").  A separate eventual
    // CONSUMER_DEREG_REQ from the consumer side is idempotent (the
    // presence is already Disconnected).
    std::vector<std::string> producer_uids;
    std::vector<std::string> consumer_uids;
    {
        std::shared_lock rlk(pImpl->mu);
        auto it = pImpl->channels.find(name);
        if (it != pImpl->channels.end())
        {
            producer_uids.reserve(it->second.producers.size());
            for (const auto &p : it->second.producers)
                if (!p.role_uid.empty()) producer_uids.push_back(p.role_uid);
            consumer_uids.reserve(it->second.consumers.size());
            for (const auto &c : it->second.consumers)
                if (!c.role_uid.empty()) consumer_uids.push_back(c.role_uid);
        }
    }

    _set_channel_closed(name);
    {
        std::unique_lock lk(pImpl->mu);
        for (const auto &producer_uid : producer_uids)
        {
            auto rit = pImpl->roles.find(producer_uid);
            if (rit == pImpl->roles.end()) continue;
            // Wave M3 step 5b: route presence termination through the
            // controlled-access API.  `on_dereg` is any-state →
            // Disconnected; idempotent if already Disconnected.
            (void)rit->second.on_dereg(name, "producer");
            // Wave M3 step 5d: cache invariant — drop the channel
            // from the role's `channels` list iff no other alive
            // presence references it.
            (void)rit->second.drop_channel_if_orphaned(name);
        }
        for (const auto &consumer_uid : consumer_uids)
        {
            auto rit = pImpl->roles.find(consumer_uid);
            if (rit == pImpl->roles.end()) continue;
            (void)rit->second.on_dereg(name, "consumer");
            (void)rit->second.drop_channel_if_orphaned(name);
        }
        pImpl->shm_blocks.erase(name);
    }
    _bump_counter(std::string("close:") + to_string(why));

    // Wave M3 step 5e (2026-05-11): schema cascade is now owner-lifetime
    // ONLY, fired via `_dispatch_role_disconnected_if_dead`.  Removed
    // the per-producer cascade that used to fire at channel-close —
    // it incorrectly evicted owner-namespaced schemas for producers
    // still alive on other channels (HEP-CORE-0034 §7.2 violation).
    //
    // Dispatch fires for every uid whose presence just transitioned
    // Disconnected.  If the role has no remaining alive presence on
    // any channel, terminal cleanup runs (schemas evict + role entry
    // erased).  If the role is still alive elsewhere, dispatch is a
    // no-op; their schemas survive intact.
    for (const auto &producer_uid : producer_uids)
        _dispatch_role_disconnected_if_dead(producer_uid);
    for (const auto &consumer_uid : consumer_uids)
        _dispatch_role_disconnected_if_dead(consumer_uid);
}

ConsumerAdmissionResult
HubState::_on_consumer_joined(const std::string&                        channel,
                              ConsumerEntry                             consumer,
                              std::optional<ChannelTopology>            declared_topology,
                              std::optional<ChannelSchemaInvariants>    open_schema,
                              std::optional<ChannelTransportInvariants> open_transport)
{
    // Grammar failures — see the parallel branch in
    // `_on_producer_added`.  Return a distinct outcome so the broker
    // adapter can map to `INVALID_REQUEST` rather than treating
    // `admitted=false` as the silent-skip (channel-vanished) contract.
    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        (!consumer.role_uid.empty() &&
         !is_valid_identifier(consumer.role_uid, IdentifierKind::RoleUid)) ||
        (open_schema.has_value() && !open_schema->schema_id.empty() &&
         !is_valid_identifier(open_schema->schema_id, IdentifierKind::Schema)))
    {
        bump_invalid_identifier(*pImpl);
        ConsumerAdmissionResult grammar_fail;
        grammar_fail.invalid_identifier = true;
        return grammar_fail;
    }

    const std::string consumer_uid = consumer.role_uid;

    auto result = _add_consumer(channel, std::move(consumer),
                                 declared_topology,
                                 open_schema, open_transport);
    if (!result.admitted) return result;

    if (!consumer_uid.empty())
    {
        RoleEntry fired = upsert_role_locked(
            *pImpl, consumer_uid, /*pubkey*/ {}, channel,
            /*role_type=*/"consumer",
            std::chrono::steady_clock::now());
        for (auto &h : snapshot_handlers(pImpl->handlers_mu, pImpl->role_reg))
            h(fired);
    }

    return result;
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
    // Route the consumer-presence termination through the M3
    // controlled-access API (`on_dereg`).  Do NOT mark the whole role
    // disconnected here — the role may still be producing on other
    // channels or attached as a consumer elsewhere.  Cache invariant
    // (`channels` contains `c` iff some alive presence references `c`)
    // is maintained via `drop_channel_if_orphaned`.  Terminal cleanup
    // (entry erase) is decided after lock release by
    // `_dispatch_role_disconnected_if_dead`.
    if (role_uid.empty()) return;
    {
        std::unique_lock lk(pImpl->mu);
        auto it = pImpl->roles.find(role_uid);
        if (it == pImpl->roles.end()) return;
        (void)it->second.on_dereg(channel, "consumer");
        (void)it->second.drop_channel_if_orphaned(channel);
    }
    _dispatch_role_disconnected_if_dead(role_uid);
}

HeartbeatEffect HubState::_on_heartbeat(const std::string                   &channel,
                             const std::string                   &role_uid,
                             const std::string                   &role_type,
                             std::chrono::steady_clock::time_point when,
                             const std::optional<nlohmann::json> &metrics)
{
    HeartbeatEffect eff_out;  // default-constructed: presence_found = false
    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        (!role_uid.empty() && !is_valid_identifier(role_uid, IdentifierKind::RoleUid)))
    {
        bump_invalid_identifier(*pImpl);
        return eff_out;
    }
    // Per HEP-CORE-0023 §2.5.2: each heartbeat refreshes ONLY the
    // matching `(uid, channel, role_type)` presence row.  Presence
    // rows are created eagerly at REG time (§2.6); a heartbeat
    // for an unknown presence is a no-op.
    if (role_uid.empty() || role_type.empty()) return eff_out;

    bool              observable_changed = false;
    ChannelEntry      fired_entry;
    ChannelObservable new_obs = ChannelObservable::kAbsent;
    {
        std::unique_lock lk(pImpl->mu);
        auto             rit = pImpl->roles.find(role_uid);
        if (rit == pImpl->roles.end()) return eff_out;

        // Route the presence-row FSM mutation through RoleEntry's
        // controlled-access API.  The method updates last_heartbeat +
        // first_heartbeat_seen, transitions FSM to Connected (legacy
        // counter name: "Ready" — see BrokerCounters docstring) if
        // not already, and returns a rich HeartbeatEffect for the
        // caller's counter decisions.  HubState retains ownership of:
        //   (a) the `pending_to_ready_total` counter bump (depends on
        //       prev_state, which only the wrapper knows; "ready"
        //       here is the legacy term for the Connected state).
        //   (b) the metrics write — still a direct presence-row
        //       mutation under the writer lock because the
        //       controlled-access API does not yet expose a
        //       per-presence metrics setter.  A future
        //       `RoleEntry::set_presence_metrics(channel, role_type,
        //       metrics)` would absorb the post-lock block below.
        const HeartbeatEffect eff =
            rit->second.on_heartbeat(channel, role_type, when);
        if (!eff.presence_found) return eff_out;
        eff_out = eff;  // surface to caller (broker layer logs first-tick)

        // Recovery from Pending counts as pending_to_ready (HEP-0023 §2.5).
        if (eff.prev_state == RoleState::Pending)
            ++pImpl->counters.pending_to_ready_total;

        // Metrics write — direct presence-row mutation under the
        // writer lock (see the (b) note on the API split above).
        if (metrics.has_value())
        {
            auto *p = rit->second.find_presence(channel, role_type);
            if (p != nullptr)
            {
                p->latest_metrics       = *metrics;
                p->metrics_collected_at = std::chrono::system_clock::now();
            }
        }

        // Fire ChannelStatusChangedHandler when the producer-presence
        // transition flips the channel's observable.  Producer-presences
        // only — consumer transitions don't affect channel observability.
        const bool transitioned_to_live =
            !eff.was_first_heartbeat_seen ||
            eff.prev_state != RoleState::Connected;
        if (role_type == "producer" && transitioned_to_live)
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
    return eff_out;
}

void HubState::_on_heartbeat_timeout(const std::string &channel,
                                     const std::string &role_uid,
                                     const std::string &role_type)
{
    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        (!role_uid.empty() && !is_valid_identifier(role_uid, IdentifierKind::RoleUid)))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    if (role_uid.empty() || role_type.empty()) return;
    // HEP-CORE-0023 §2.1: per-presence Connected → Pending.  The role
    // is NOT marked Disconnected here — Pending means "suspicious, may
    // recover via the next heartbeat".  Disconnection (if any) happens
    // at `_on_pending_timeout`.  Channel observability (and the
    // ChannelStatusChangedHandler fan-out) tracks producer presence
    // only; consumer-presence transitions update the FSM + counter but
    // do not affect channel state.
    const bool        is_producer  = (role_type == "producer");
    bool              transitioned = false;
    ChannelEntry      fired_entry;
    ChannelObservable new_obs = ChannelObservable::kAbsent;
    {
        std::unique_lock lk(pImpl->mu);
        auto rit = pImpl->roles.find(role_uid);
        if (rit == pImpl->roles.end()) return;

        // Wave M3 step 3: route FSM transition through the controlled-
        // access API.  `first_heartbeat_seen` is NOT a gate — the
        // registered-but-never-heartbeat case demotes via this same
        // path once `last_heartbeat` (stamped at REG_REQ time) ages
        // past ready_timeout.
        const TransitionEffect te =
            rit->second.on_heartbeat_timeout(channel, role_type);
        if (te != TransitionEffect::ToPending) return;
        ++pImpl->counters.ready_to_pending_total;
        transitioned = true;

        if (is_producer)
        {
            auto it = pImpl->channels.find(channel);
            if (it != pImpl->channels.end())
            {
                fired_entry = it->second;
                new_obs     = compute_channel_observable(it->second, pImpl->roles);
            }
        }
    }
    if (transitioned && is_producer)
    {
        for (auto &h : snapshot_handlers(pImpl->handlers_mu,
                                         pImpl->ch_status_changed))
            h(fired_entry, new_obs);
    }
}

RemoveProducerResult
HubState::_on_pending_timeout(const std::string &channel,
                               const std::string &role_uid,
                               const std::string &role_type)
{
    RemoveProducerResult result{false, false};

    if (!is_valid_identifier(channel, IdentifierKind::Channel) ||
        !is_valid_identifier(role_uid, IdentifierKind::RoleUid))
    {
        bump_invalid_identifier(*pImpl);
        return result;
    }
    if (role_type.empty()) return result;

    // ── Consumer-presence path ─────────────────────────────────────
    // HEP-CORE-0023 §2.1 + §2.1.1: consumer-presence Pending →
    // Disconnected does NOT tear down the channel — only the
    // producer side controls channel observability + teardown.  Erase
    // the consumer slot from `ChannelEntry.consumers[]`, run the
    // role's `drop_channel_if_orphaned` cache cleanup, and dispatch
    // the role-disconnect cascade in case this was the role's last
    // alive presence anywhere.
    if (role_type == "consumer")
    {
        bool eligible = false;
        {
            std::unique_lock lk(pImpl->mu);
            auto it = pImpl->channels.find(channel);
            if (it == pImpl->channels.end()) return result;

            auto rit = pImpl->roles.find(role_uid);
            if (rit == pImpl->roles.end()) return result;

            const TransitionEffect te =
                rit->second.on_pending_timeout(channel, "consumer");
            if (te != TransitionEffect::ToDisconnected) return result;
            ++pImpl->counters.pending_to_deregistered_total;
            eligible = true;

            // `remove_consumer` is best-effort: a consumer slot may
            // not exist on `ChannelEntry.consumers[]` for every
            // consumer-presence (e.g., presence created without an
            // accompanying CONSUMER_REG_REQ).  Reflect the actual
            // mutation in `result.removed`.
            result.removed           = it->second.remove_consumer(role_uid);
            result.channel_now_empty = false;
            (void)rit->second.drop_channel_if_orphaned(channel);
        }
        if (eligible) _dispatch_role_disconnected_if_dead(role_uid);
        return result;
    }

    // ── Producer-presence path (existing behavior) ─────────────────
    // HEP-CORE-0023 §2.1 + §2.1.1: producer-presence Pending →
    // Disconnected; atomic channel teardown fires ONLY on the LAST
    // producer's transition.  No grace window, no Closing state.
    //
    // The producer-presence's `Pending` state is the single-shot gate
    // — a concurrent timer fire that loses the writer-lock race
    // observes "presence absent" (post-H18 erase) and bails without
    // re-entering teardown or double-bumping the counter.
    //
    // Ordering note: in the LAST-producer path we leave the producer
    // in `ChannelEntry.producers[]` so `_on_channel_closed` can see
    // the uid in its `producer_uids` snapshot and dispatch
    // role-disconnect terminal cleanup for it.  Removal-before-
    // close-cascade would empty `producers[]` and skip the dispatch
    // for this uid, leaking the role entry.  Same bug class as Wave
    // M2.5 step 4 fix; updated for Wave M3 step 5e (H12) which moved
    // schema eviction from `_on_channel_closed` directly into the
    // dispatch path (cascade_role_terminal_cleanup_locked).
    bool is_last_producer = false;
    bool eligible         = false;
    {
        std::unique_lock lk(pImpl->mu);
        auto             it = pImpl->channels.find(channel);
        if (it == pImpl->channels.end()) return result;
        if (it->second.find_producer(role_uid) == nullptr) return result;

        auto rit = pImpl->roles.find(role_uid);
        if (rit == pImpl->roles.end()) return result;

        // Wave M3 step 3: route FSM transition through the controlled-
        // access API.  `on_pending_timeout` is a no-op when the
        // presence is not Pending — handles the lost-race case
        // (concurrent timer fires) without re-entering teardown.
        const TransitionEffect te =
            rit->second.on_pending_timeout(channel, "producer");
        if (te != TransitionEffect::ToDisconnected) return result;
        ++pImpl->counters.pending_to_deregistered_total;
        eligible         = true;
        is_last_producer = (it->second.producer_count() == 1);
        if (!is_last_producer)
        {
            // Multi-producer channel — drop just this one; channel
            // survives.  Producer-presence FSM already transitioned
            // Disconnected above via on_pending_timeout; maintain the
            // `channels` cache invariant (Wave M3 step 5d).
            auto rm                  = it->second.remove_producer(role_uid);
            result.removed           = rm.removed;
            result.channel_now_empty = false;
            (void)rit->second.drop_channel_if_orphaned(channel);
            // HEP-CORE-0042 §5.4: kDead heartbeat is a normative
            // reset trigger for confirmed_version[K][P] (same
            // discipline as VoluntaryDereg in _on_producer_dropped).
            // Held under the same writer lock as remove_producer so
            // the state triple stays consistent for readers.
            auto acc_it = pImpl->channel_access_index.find(channel);
            if (acc_it != pImpl->channel_access_index.end())
            {
                acc_it->second.ledger.reset_role_confirmation(role_uid);
            }
            // Fall through to the dispatch step.
        }
        // Last-producer path: leave the producer in producers[] so
        // `_on_channel_closed`'s producer_uids snapshot includes
        // this uid → dispatch fires terminal cleanup for the role.
    }

    if (!eligible) return result;
    if (is_last_producer)
    {
        // Atomic channel teardown — `_on_channel_closed` dispatches
        // role-disconnect terminal cleanup for every producer uid
        // (Wave M3 step 5b, 2026-05-11).
        _on_channel_closed(channel, ChannelCloseReason::HeartbeatTimeout);
        result.removed           = true;
        result.channel_now_empty = true;
    }
    else
    {
        // Multi-producer path: this producer's presence just went
        // Disconnected.  Dispatch terminal cleanup in case this was
        // the role's last alive presence anywhere.
        _dispatch_role_disconnected_if_dead(role_uid);
    }
    return result;
}

// M1.4 (2026-05-11): `HubState::_on_metrics_reported` deleted.
// Metrics arrive only via `_on_heartbeat` per HEP-CORE-0019 §2.3
// Phase 6.  The dedicated time-only METRICS_REPORT_REQ wire path
// is retired.

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
    // Voluntary BAND_LEAVE_REQ — pass reason="voluntary" through to
    // the handler so the broker subscriber emits the correct wire
    // reason in BAND_LEAVE_NOTIFY.
    _set_band_left(band, role_uid, "voluntary");
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

// ─── Channel-access capability ops (HEP-CORE-0036 §4.1) ─────────────────────

void HubState::_on_channel_access_opened(const std::string &channel_name)
{
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    std::unique_lock lk(pImpl->mu);
    // Idempotent open: if the channel already has an access record,
    // do not overwrite — the existing allowlist is the canonical record.
    pImpl->channel_access_index.try_emplace(channel_name, ChannelAccessEntry{});
}

void HubState::_on_channel_access_closed(const std::string &channel_name)
{
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel))
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    std::unique_lock lk(pImpl->mu);
    pImpl->channel_access_index.erase(channel_name);
}

void HubState::_on_consumer_authorized(const std::string &channel_name,
                                        const std::string &pubkey_z85)
{
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel) ||
        pubkey_z85.empty())
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    std::unique_lock lk(pImpl->mu);
    auto it = pImpl->channel_access_index.find(channel_name);
    if (it == pImpl->channel_access_index.end())
    {
        return;  // no-op per contract
    }
    // Ledger's `admit` is idempotent (no-bump on re-admit) and captures
    // the admission version.  Later `is_visible_to` calls use that
    // version to gate visibility per HEP-CORE-0042 §5.5.2
    // INVARIANT-BIND-CONFIRM-2.
    it->second.ledger.admit(pubkey_z85);
}

void HubState::_on_consumer_revoked(const std::string &channel_name,
                                     const std::string &pubkey_z85)
{
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel) ||
        pubkey_z85.empty())
    {
        bump_invalid_identifier(*pImpl);
        return;
    }
    std::unique_lock lk(pImpl->mu);
    auto it = pImpl->channel_access_index.find(channel_name);
    if (it == pImpl->channel_access_index.end())
    {
        return;  // no-op per contract
    }
    // Ledger's `revoke` always bumps current_version (revocation is a
    // state-change signal for role confirmations to catch up to).
    // Removes `pubkey_z85` from the admission map entirely — subsequent
    // `is_visible_to` returns false regardless of confirmed_version,
    // closing the revocation race by construction (no stale
    // "confirmed" snapshot to unlearn from).
    it->second.ledger.revoke(pubkey_z85);
}

std::uint64_t
HubState::producer_instance(const std::string &producer_role_uid) const
{
    if (producer_role_uid.empty()) return 0;
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->producer_instance.find(producer_role_uid);
    return (it == pImpl->producer_instance.end()) ? 0 : it->second;
}

bool
HubState::nonce_seen(std::string_view role_uid,
                      std::string_view client_nonce,
                      std::uint64_t     wall_ts,
                      std::uint64_t     window_ms)
{
    // I-REPLAY-BOUND: freshness check + record, atomic under the
    // HubState writer lock.  Returns true when the nonce is fresh
    // (added to the dedup set); false on collision within the window.
    //
    // Callers upstream (broker admission handlers) have already
    // rejected wall-clock skew as a distinct reject class; here we
    // only need to worry about nonce reuse.
    if (role_uid.empty() || client_nonce.empty()) return false;

    std::unique_lock lk(pImpl->mu);
    const std::string role_uid_key{role_uid};
    auto &entries = pImpl->nonce_dedup[role_uid_key];

    // Prune-on-access: drop entries older than (wall_ts - window_ms).
    // Underflow-safe: if window_ms > wall_ts, cutoff would wrap; use 0
    // as the effective floor.
    const std::uint64_t cutoff =
        (wall_ts > window_ms) ? (wall_ts - window_ms) : 0;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                  [cutoff](const Impl::NonceEntry &e) {
                                      return e.wall_ts < cutoff;
                                  }),
                   entries.end());

    // Membership check on the pruned window.
    for (const auto &e : entries)
    {
        if (e.nonce == client_nonce) return false;  // duplicate
    }

    Impl::NonceEntry fresh;
    fresh.nonce.assign(client_nonce);
    fresh.wall_ts = wall_ts;
    entries.push_back(std::move(fresh));
    return true;
}

std::uint64_t
HubState::_on_role_confirmed(const std::string &channel_name,
                              const std::string &role_uid,
                              std::uint64_t      applied_version)
{
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel) ||
        role_uid.empty())
    {
        bump_invalid_identifier(*pImpl);
        return 0;
    }
    std::unique_lock lk(pImpl->mu);
    auto it = pImpl->channel_access_index.find(channel_name);
    if (it == pImpl->channel_access_index.end())
    {
        return 0;
    }
    // Ledger's `confirm` is monotonic (max(current, applied_version));
    // stale APPLIED_REQ wires are silently absorbed.  Works for BOTH
    // the binding-side role (was `_on_binding_confirmed` — that set-
    // snapshot path is retired) AND the dialing-side producer's own
    // confirmed_version (§5.4 fast-path callers keep working via the
    // same primitive).
    return it->second.ledger.confirm(role_uid, applied_version);
}

bool
HubState::is_role_registered_on_channel(const std::string &channel_name,
                                          const std::string &role_uid,
                                          const std::string &role_type) const
{
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel) ||
        role_uid.empty())
    {
        return false;
    }
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->channels.find(channel_name);
    if (it == pImpl->channels.end())
    {
        return false;
    }
    const auto &ch = it->second;
    if (role_type == "consumer")
    {
        return std::any_of(
            ch.consumers.begin(), ch.consumers.end(),
            [&](const ConsumerEntry &c) { return c.role_uid == role_uid; });
    }
    if (role_type == "producer")
    {
        return std::any_of(
            ch.producers.begin(), ch.producers.end(),
            [&](const ProducerEntry &p) { return p.role_uid == role_uid; });
    }
    return false;
}

std::optional<bool>
HubState::is_pubkey_visible_to(const std::string &channel_name,
                                const std::string &binding_role_uid,
                                const std::string &pubkey_z85) const
{
    if (!is_valid_identifier(channel_name, IdentifierKind::Channel) ||
        binding_role_uid.empty() || pubkey_z85.empty())
    {
        return std::nullopt;
    }
    std::shared_lock lk(pImpl->mu);
    auto it = pImpl->channel_access_index.find(channel_name);
    if (it == pImpl->channel_access_index.end())
    {
        return std::nullopt;
    }
    return it->second.ledger.is_visible_to(binding_role_uid, pubkey_z85);
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
