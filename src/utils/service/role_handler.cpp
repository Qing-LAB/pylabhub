/**
 * @file role_handler.cpp
 * @brief Wave-B M3 skeleton — RoleHandler dedup + index build.
 *
 * Pure logic; no network, no threading, no allocations of the
 * `BrokerRequestComm` pImpl (that arrives in Wave-B M4 with
 * `start()`/`shutdown()`).  See `role_handler.hpp` for the
 * construction contract + invariants.
 */

#include "utils/role_handler.hpp"

#include "utils/logger.hpp"

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
            continue;  // unbound presences are not indexed.

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

}  // namespace pylabhub::scripting
