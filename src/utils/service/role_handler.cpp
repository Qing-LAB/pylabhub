/**
 * @file role_handler.cpp
 * @brief RoleHandler dedup + index build (M3) + network state ops (M4a).
 *
 * Per HEP-CORE-0019 + role_host_template_design.md §5, RoleHandler is
 * a state holder + routing helper:
 *   - M3: topology (presences) + dedup (connections) + indexes.
 *   - M4a: per-connection BRC allocation + connect/disconnect.
 *   - M4b: four-class dispatch (send_class_A/B/C/D).
 *
 * Threading is NOT inside RoleHandler.  The role host (via
 * RoleAPIBase's ThreadManager) is the action taker: AFTER
 * `start_connections()` returns, the host spawns one ctrl thread per
 * BRC to drive its poll loop.  See `role_handler.hpp` § "Network
 * state lifecycle" for the responsibility split.
 */

#include "utils/role_handler.hpp"

#include "utils/broker_request_comm.hpp"
#include "utils/logger.hpp"
#include "utils/role_api_base.hpp"

namespace pylabhub::scripting
{

RoleHandler::RoleHandler(std::vector<Presence> presences)
    : presences_(std::move(presences))
{
    build_connections_();
    build_channel_index_();
}

void RoleHandler::build_connections_()
{
    // Single-pass dedup by `(broker_endpoint, broker_pubkey)`.  Order
    // is "first presence wins" — the first presence in the declared
    // list that names a given hub identity creates the HubConnection;
    // subsequent presences naming the same hub bind to that slot.
    //
    // Vector size is bounded by `presences_.size()` (worst case: every
    // presence on a distinct hub).  Reserve up front so we can safely
    // hold `HubConnection *` from `presences_[i].connection` without
    // reallocation invalidating those pointers.
    //
    // CRITICAL: `connections_` must NOT be mutated after this build
    // loop returns.  Every Presence in `presences_` will hold a raw
    // `HubConnection *` into this vector; any subsequent `emplace_back`
    // /  `erase` / `clear` / `resize` on `connections_` could trigger
    // reallocation and silently dangle every Presence::connection
    // pointer.  See `role_handler.hpp` § "Immutability contract" for
    // the full pointer-stability rationale; future hub-failover work
    // must re-architect to a stable-storage container before allowing
    // post-build mutation.
    connections_.reserve(presences_.size());

    for (auto &p : presences_)
    {
        // Linear scan over `connections_` — O(presences²) worst case,
        // but presence_count is small (≤ 3 in current designs, bounded
        // by topology not data volume).  No hash key needed.
        HubConnection *match = nullptr;
        for (auto &c : connections_)
        {
            if (c.broker_endpoint == p.hub.broker &&
                c.broker_pubkey   == p.hub.broker_pubkey)
            {
                match = &c;
                break;
            }
        }
        if (match == nullptr)
        {
            connections_.emplace_back(p.hub.broker, p.hub.broker_pubkey);
            match = &connections_.back();
        }
        p.connection = match;
    }
}

void RoleHandler::build_channel_index_()
{
    channel_index_.reserve(presences_.size());

    for (auto &p : presences_)
    {
        if (p.channel.empty())
            continue;
        // Unbound presences (empty channel name) are NOT indexed.  In
        // current topologies the caller passes channel-bound presences
        // at ctor time, so this path is unused.  It exists as a
        // forward-compat hook for future phases where a presence may
        // be declared before its channel is resolved (e.g.,
        // schema-resolve-driven late binding) — those phases will need
        // to call `find_presence_for_channel` only AFTER binding
        // completes.  Today, an empty-channel presence in production
        // input is a caller bug; we skip-and-continue rather than
        // throw so the rest of the index still builds.

        // Caller-responsibility invariant: no two presences on this
        // role share the same channel name.  See file-header §
        // "Wave-B M3 invariants verified by L2 tests".  We log + skip
        // duplicates rather than throw — the L2 tests pin the count
        // via `presence_count_for_channel` so a mistake at the call
        // site fails loudly without needing exception propagation.
        const auto it = channel_index_.find(p.channel);
        if (it == channel_index_.end())
        {
            channel_index_.emplace(p.channel, &p);
        }
        else
        {
            LOGGER_ERROR("RoleHandler: duplicate presence on channel '{}' — "
                         "the second entry is not indexed (callers must "
                         "ensure presences within a role have distinct "
                         "channels per HEP-CORE-0033 §19)",
                         p.channel);
        }
    }
}

const Presence *
RoleHandler::find_presence_for_channel(const std::string &channel) const noexcept
{
    auto it = channel_index_.find(channel);
    return (it == channel_index_.end()) ? nullptr : it->second;
}

std::size_t
RoleHandler::presence_count_for_channel(const std::string &channel) const noexcept
{
    std::size_t n = 0;
    for (const auto &p : presences_)
        if (p.channel == channel)
            ++n;
    return n;
}

// ============================================================================
// Network state lifecycle (Wave-B M4a)
// ============================================================================
//
// STATE-only.  These methods manipulate BRC resources owned by each
// HubConnection — they do NOT manage threads.  See role_handler.hpp
// § "Network state lifecycle" for the contract + the responsibility
// split between handler (state) and role host (execution).

bool RoleHandler::connections_started() const noexcept
{
    // Derived state: BRCs are non-null iff start_connections succeeded.
    // start_connections is all-or-nothing (rolls back on partial fail),
    // so checking the first slot is sufficient for the steady-state
    // invariant.  Empty handler (no connections) is "not started" by
    // this definition — there is nothing to start.
    return !connections_.empty() && connections_[0].brc != nullptr;
}

bool RoleHandler::start_connections(const RoleAPIBase &owner)
{
    if (connections_started())
    {
        // Programmer-error refusal — caller learns via the return
        // value.  WARN (not ERROR) because the handler state stays
        // consistent: connections remain in their previously-started
        // state, no resources leak.
        LOGGER_WARN("RoleHandler::start_connections called twice without "
                    "intervening stop_connections — refusing");
        return false;
    }

    // ── Per-HubConnection: allocate + connect ─────────────────────────────
    //
    // BrokerRequestComm::Config required fields (broker_request_comm.hpp
    // §"Configuration"):
    //   broker_endpoint, broker_pubkey, client_pubkey, client_seckey,
    //   role_uid, role_name.
    // Per-HubConnection (broker_endpoint/broker_pubkey) come from the
    // dedup identity; role-wide fields come from `owner`.
    for (auto &c : connections_)
    {
        c.brc = std::make_unique<hub::BrokerRequestComm>();

        hub::BrokerRequestComm::Config cfg;
        cfg.broker_endpoint = c.broker_endpoint;
        cfg.broker_pubkey   = c.broker_pubkey;
        cfg.client_pubkey   = owner.auth_client_pubkey();
        cfg.client_seckey   = owner.auth_client_seckey();
        cfg.role_uid        = owner.uid();
        cfg.role_name       = owner.name();

        if (!c.brc->connect(cfg))
        {
            // Log uid + pubkey (safe); never log seckey.
            LOGGER_ERROR("RoleHandler: BRC connect failed for hub '{}' "
                         "(role_uid='{}')",
                         c.broker_endpoint, owner.uid());
            // Roll back: any previously-connected BRCs in this loop get
            // disconnected + released so we're back to the pre-start
            // state.  No poll loops have been spawned (handler doesn't
            // own threads), so `disconnect()` alone is sufficient.
            stop_connections();
            return false;
        }
    }

    return true;
}

void RoleHandler::stop_connections() noexcept
{
    for (auto &c : connections_)
    {
        if (!c.brc) continue;
        c.brc->disconnect();
        c.brc.reset();
    }
}

}  // namespace pylabhub::scripting
