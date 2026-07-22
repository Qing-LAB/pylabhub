#pragma once
/**
 * @file hub_connection.hpp
 * @brief Wave-B M3 skeleton — one physical broker connection.
 *
 * A `HubConnection` is a single `(BrokerRequestComm, ctrl thread, ZMTP
 * socket monitor)` triple identifying ONE physical connection to a
 * broker.  Multiple presences MAY share one connection when their
 * resolved `(broker_endpoint, broker_pubkey)` pair is identical — the
 * dedup happens at `RoleHandler` construction (design §5.4).
 *
 * Wave-B M3 ships this as a header-only data type — no allocations,
 * no methods that touch the network.  The `brc` unique_ptr remains
 * nullptr through M3 (tests verify dedup + index shape without
 * instantiating BRC).  M4 swaps `RoleAPIBase::pImpl` to materialise
 * one BRC per HubConnection at startup; M8 enables N-connection
 * deployments end-to-end.
 *
 * Identity (`broker_endpoint`, `broker_pubkey`) is the dedup key.
 * Equality on this pair is the canonical "same hub" predicate used
 * by `RoleHandler::build_connections_`.
 */

#include "utils/broker_request_comm.hpp"

#include <memory>
#include <string>

namespace pylabhub::scripting
{

/// One physical connection to a broker.  Owned by
/// `RoleHandler::connections_` (vector-of-value); presences hold raw
/// `HubConnection*` into that vector.  The vector contents are stable
/// for the RoleHandler's lifetime — connections are not added or
/// removed mid-flight (no rebalancing across hub failover).
///
/// Wave-B M3 leaves `brc` nullptr — the actual `BrokerRequestComm`
/// allocation + connect happens at `RoleHandler::start()` in Wave-B M4.
class HubConnection
{
  public:
    /// Construction-only identity used as the dedup key.  Two
    /// `HubConnection` instances are interchangeable iff this pair is
    /// bitwise-equal.
    std::string broker_endpoint;
    std::string broker_pubkey;

    /// The actual ZMQ DEALER + protocol machinery.  Wave-B M3 leaves
    /// this nullptr; Wave-B M4 allocates + `connect()`s it during
    /// `RoleHandler::start()`.
    std::unique_ptr<hub::BrokerRequestComm> brc;

    HubConnection(std::string endpoint, std::string pubkey) noexcept
        : broker_endpoint(std::move(endpoint)), broker_pubkey(std::move(pubkey))
    {
    }

    HubConnection(const HubConnection &) = delete;
    HubConnection &operator=(const HubConnection &) = delete;
    HubConnection(HubConnection &&) noexcept = default;
    HubConnection &operator=(HubConnection &&) noexcept = default;
};

} // namespace pylabhub::scripting
