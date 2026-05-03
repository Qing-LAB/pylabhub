#pragma once
/**
 * @file hub_state_json.hpp
 * @brief JSON serializers for HubState entry types.
 *
 * Free functions that convert each `HubState` entry struct
 * (`ChannelEntry`, `RoleEntry`, `BandEntry`, `PeerEntry`,
 * `BrokerCounters`) to a `nlohmann::json` object.  Used by:
 *   - `BrokerService::query_metrics()` — builds METRICS_ACK responses.
 *   - `AdminService` (HEP-CORE-0033 §11.2) — query RPC responses.
 *
 * Promoted from a private anonymous namespace in `broker_service.cpp`
 * to a shared header during HEP-0033 Phase 6.2b so the admin RPC
 * dispatch can reuse the same on-the-wire shape — keeping a single
 * source of truth for these field layouts (any future field addition
 * surfaces in both metrics and admin responses without drift).
 *
 * The shapes are stable wire contracts:
 *   - Field names + types are preserved across versions.
 *   - New fields append; existing fields keep their meaning.
 *   - Time-typed fields are emitted as RFC3339-like strings via
 *     `format_tools::formatted_time` (system_clock) or
 *     `<X>_ms_ago` ints (steady_clock — relative-age signal,
 *     no wall-clock guess).
 */

#include "utils/hub_state.hpp"

#include <nlohmann/json.hpp>

namespace pylabhub::hub
{

/// Serialize a single channel entry.  Includes the producer block,
/// schema metadata, transport endpoints, and all attached consumers.
[[nodiscard]] nlohmann::json
channel_to_json(const ChannelEntry &c);

/// Serialize a registered role.  Includes the latest pushed metrics
/// payload so callers don't have to issue a separate fetch.
[[nodiscard]] nlohmann::json
role_to_json(const RoleEntry &r);

/// Serialize a band (HEP-CORE-0030).  Members are emitted in stored
/// order with `joined_ms_ago` instead of an absolute timestamp.
[[nodiscard]] nlohmann::json
band_to_json(const BandEntry &b);

/// Serialize a federation peer.  Excludes the raw ZMQ routing
/// identity (broker-internal — not for off-wire consumers).
[[nodiscard]] nlohmann::json
peer_to_json(const PeerEntry &p);

/// Serialize the broker counters block.  Output preserves the full
/// `msg_type_counts` / `msg_type_errors` maps without filtering.
[[nodiscard]] nlohmann::json
broker_counters_to_json(const BrokerCounters &c);

} // namespace pylabhub::hub
