#pragma once
/**
 * @file role_presence.hpp
 * @brief Per-role presence model — Wave-B M3 skeleton (build-only).
 *
 * A `Presence` is a single `(hub, channel, role_kind, schemas, inbox)`
 * tuple identifying ONE registration that a role maintains.  A role's
 * presence list is the canonical description of "what this role does
 * on which hubs":
 *
 *   Producer:                    [{ out_hub, out_channel, producer }]
 *   Consumer:                    [{ in_hub,  in_channel,  consumer }]
 *   Processor (single-hub):      [{ hub, in_channel,  consumer },
 *                                  { hub, out_channel, producer }]
 *   Processor (dual-hub):        [{ in_hub,  in_channel,  consumer },
 *                                  { out_hub, out_channel, producer }]
 *   Future N-input router:       [{ hubA, chA, consumer },
 *                                  { hubB, chB, consumer },
 *                                  { hubC, chC, producer }]
 *
 * Per HEP-CORE-0033 §19 + role_host_template_design.md §5.  The
 * presence-list ABSTRACTION removes the hardcoded single-channel +
 * single-broker-connection assumptions in today's RoleAPIBase and is
 * the structural pre-requisite for Wave-B M8 (dual-hub processor).
 *
 * Wave-B M3 ships this as a HEADER-ONLY data type — no allocations,
 * no network methods.  M4 swaps `RoleAPIBase::pImpl` to delegate via
 * `RoleHandler` (which owns the presence vector).  M5..M9 migrate
 * the role kinds and collapse `worker_main_` into the template.
 */

#include "utils/config/hub_ref_config.hpp"

#include <cstdint>
#include <string>

namespace pylabhub::scripting
{

/// Wire-protocol presence kind (HEP-CORE-0019 §4.1 `role_type` field
/// values).  Maps 1:1 to the string sent on `HEARTBEAT_REQ`:
///   - `RoleKind::Producer` ↔ `"producer"`
///   - `RoleKind::Consumer` ↔ `"consumer"`
/// Processor roles do not have a distinct wire `role_kind` — a
/// processor holds TWO presences, each one Producer or Consumer in
/// kind (see file-header presence-list shapes).
enum class RoleKind : std::uint8_t
{
    Producer = 1,
    Consumer = 2,
};

/// Convert a `RoleKind` to its wire-protocol string per HEP-CORE-0019
/// §4.1.  Used by Wave-B M4b dispatch when populating `role_type`
/// fields on outbound wire payloads.
[[nodiscard]] inline const char *to_wire_string(RoleKind k) noexcept
{
    switch (k)
    {
    case RoleKind::Producer: return "producer";
    case RoleKind::Consumer: return "consumer";
    }
    return "";  // unreachable: switch is exhaustive over RoleKind values
}

class HubConnection;

/// One presence row this role maintains.  A role's `RoleHandler` owns
/// a `std::vector<Presence>` declared at startup; the order matches
/// the per-role-type presence-list shapes documented at the top of
/// this file.  Fields are populated incrementally:
///
///  - `hub`, `channel`, `role_kind`  set at construction (caller
///                                   knows the topology).
///  - `connection`                   set during dedup at
///                                   `RoleHandler::build_connections_()`;
///                                   NEVER reassigned after that.
///
/// Schema / inbox metadata (slot_spec, fz_spec, inbox_meta in the
/// design §5.6 sketch) are intentionally OMITTED until Wave-B M5+
/// — they're set during the role host's schema-resolve step.
/// Adding them in M4 would be anticipatory; M5 reintroduces them
/// alongside the call sites that need them.
///
/// Equality + sort are defined by `(hub.broker, hub.broker_pubkey,
/// channel, role_kind)` — the natural identity tuple per
/// HEP-CORE-0033 §19.  Two presences with the same tuple on the same
/// role are forbidden by construction.
struct Presence
{
    config::HubRefConfig hub;        ///< Resolved (broker, broker_pubkey).
    std::string          channel;    ///< Channel name on `hub`.
    RoleKind             role_kind{RoleKind::Producer};

    /// Non-owning pointer into the owning `RoleHandler::connections_`
    /// vector.  Set during dedup; nullptr before then.  The pointer is
    /// stable for the RoleHandler's lifetime (connections vector is
    /// not mutated after build).
    HubConnection *connection{nullptr};
};

}  // namespace pylabhub::scripting
