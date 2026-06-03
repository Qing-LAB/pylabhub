#pragma once
/**
 * @file role_presence.hpp
 * @brief Per-role presence model — `Presence` row + `RegistrationState` FSM.
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
 * Per HEP-CORE-0033 §19 + docs/archive/transient-2026-06-02/role_host_template_design.md §5.  The
 * presence-list ABSTRACTION removes the hardcoded single-channel +
 * single-broker-connection assumptions in today's RoleAPIBase and is
 * the structural pre-requisite for Wave-B M8 (dual-hub processor).
 *
 * **History.**  Originally shipped in Wave-B M3 as a header-only
 * skeleton (no allocations, no network methods).  M4 added the
 * `HubConnection *` non-owning pointer set during dedup at
 * `RoleHandler::build_connections_()`.  **Audit S1+O4 (2026-05-17)**
 * added the per-presence registration FSM (`RegistrationState` enum
 * + atomic field below) — this replaced the prior implicit
 * "registered if the channel string is non-empty in
 * `Impl::Shared`" inference with an explicit four-state machine.
 * The atomic field forced an explicit move ctor (atomics are not
 * trivially movable); both the field and the move semantics are
 * documented inline.
 */

#include "utils/config/hub_ref_config.hpp"
#include "utils/schema_types.hpp"   // hub::SchemaSpec (per-presence slot/fz schemas)

#include <atomic>
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

/// Role-side registration FSM for a single Presence (audit S1+O4,
/// 2026-05-17).  Per HEP-CORE-0023 §2 + the Round-2 audit, pre-S1 the
/// role had no enumerable registration state — it was implicit in the
/// non-emptiness of `Impl::Shared::producer_channel` /
/// `consumer_channel`, which couldn't distinguish "never registered"
/// from "registered then deregistered" and didn't observe the
/// in-flight window.  This enum makes the four legitimate states
/// explicit on the Presence record:
///
///   Unregistered         — initial state at handler construction; no
///                          REG_REQ has been attempted for this
///                          presence yet.
///   RegRequestPending    — REG_REQ / CONSUMER_REG_REQ has been
///                          dispatched; we are waiting for the broker
///                          to ACK.  Observable: the role-side
///                          intends to register but the broker has
///                          not yet confirmed.
///   Registered           — broker returned a success ACK; the
///                          presence is admitted into HubState and
///                          counts as a member of its channel.
///   Deregistered         — voluntary DEREG_REQ / CONSUMER_DEREG_REQ
///                          has succeeded (or the broker disappeared
///                          mid-teardown so we cleared our side
///                          unilaterally).  Terminal for this
///                          process; the presence's Presence row
///                          stays in `handler_->presences()` for
///                          identity/dedup purposes but no longer
///                          drives broker traffic.
///
/// Mapping to the user's "explicit and confirmed state without
/// ambiguity" requirement (Round-2 R2.1 S1): callers ask
/// `presence.registration_state.load()` and get a single enum
/// instead of inferring state from two string fields' non-emptiness.
enum class RegistrationState : std::uint8_t
{
    Unregistered      = 0,
    RegRequestPending = 1,
    Registered        = 2,
    Deregistered      = 3,
};

[[nodiscard]] inline const char *to_string(RegistrationState s) noexcept
{
    switch (s)
    {
    case RegistrationState::Unregistered:      return "Unregistered";
    case RegistrationState::RegRequestPending: return "RegRequestPending";
    case RegistrationState::Registered:        return "Registered";
    case RegistrationState::Deregistered:      return "Deregistered";
    }
    return "<unknown>";
}

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
///  - `hub`, `channel`, `role_kind`, `slot_spec`, `fz_spec`
///                                   set at construction time by the
///                                   role's `build_presences_()`
///                                   override.
///                                   The override resolves schemas
///                                   from on-disk files using the
///                                   presence's own `hub.hub_dir`
///                                   as the schema search path.
///  - `connection`                   set during dedup at
///                                   `RoleHandler::build_connections_()`;
///                                   NEVER reassigned after that.
///
/// **Per-presence schemas**:
/// `slot_spec` and `fz_spec` are the parsed `SchemaSpec` for THIS
/// presence's channel.  They were previously stored as per-direction
/// members on the role host (`in_slot_spec_` / `out_slot_spec_`) and
/// as `in_fz_spec_` / `out_fz_spec_` on `RoleHostCore`.  The
/// fragmentation was an artifact of incremental design (this header's
/// historical comment noted "M5+ reintroduces them alongside the call
/// sites that need them").  M9's role-host unification IS that call
/// site.  Now each `Presence` carries its own schemas — single home,
/// per-channel.  Future multi-rx / multi-tx roles (router, etc.) will
/// have N presences each with their own schemas; no further structural
/// change needed.
///
/// Equality + sort are defined by `(hub.broker, hub.broker_pubkey,
/// channel, role_kind)` — the natural identity tuple per
/// HEP-CORE-0033 §19.  Two presences with the same tuple on the same
/// role are forbidden by construction.  Schemas are NOT part of the
/// identity tuple (one channel = one schema in any valid
/// configuration).
struct Presence
{
    config::HubRefConfig hub;        ///< Resolved (broker, broker_pubkey).
    std::string          channel;    ///< Channel name on `hub`.
    RoleKind             role_kind{RoleKind::Producer};

    /// Parsed schema for the data slot on this presence's channel.
    /// Resolved by the role's `build_presences_()` from the config's
    /// slot-schema JSON (e.g. `out_slot_schema_json` for a Producer
    /// presence, `in_slot_schema_json` for a Consumer presence) using
    /// `hub::resolve_schema()`.  Consumed by:
    ///   - `make_*_opts` (queue construction) via
    ///     `pylabhub::scripting::make_tx_opts` /
    ///     `pylabhub::scripting::make_rx_opts`.
    ///   - `compute_*_message_info()` for engine-init params.
    ///   - `RoleHostCore::set_*_slot_size()` (size cached for hot path).
    hub::SchemaSpec      slot_spec;

    /// Parsed schema for the optional flexzone on this presence's
    /// channel.  May be empty (`!has_schema`) if the role declares
    /// no flexzone.  Same consumers as `slot_spec`.
    /// `fz_spec.has_schema` replaces the historical
    /// `RoleHostCore::has_rx_fz()` / `has_tx_fz()` accessors — count
    /// presences whose `role_kind` matches and whose `fz_spec.has_schema`
    /// is true.
    hub::SchemaSpec      fz_spec;

    /// Non-owning pointer into the owning `RoleHandler::connections_`
    /// vector.  Set during dedup; nullptr before then.  The pointer is
    /// stable for the RoleHandler's lifetime (connections vector is
    /// not mutated after build).
    HubConnection *connection{nullptr};

    /// Per-presence registration FSM (audit S1+O4, 2026-05-17).
    /// Mutated only from the role's caller threads
    /// (`register_*` succeed/fail paths and `deregister_from_broker`
    /// teardown); atomic to give defensible read semantics from any
    /// thread that probes state (script handlers, admin RPCs, future
    /// observers).  See `RegistrationState` docstring above for the
    /// four states and HEP-CORE-0023 §2 cross-reference.
    ///
    /// Default `Unregistered` at construction.  No `Created` /
    /// `Connecting` / etc. — those concerns live on
    /// `HubConnection`; this state is strictly about REG_REQ /
    /// DEREG_REQ admission with the broker.
    std::atomic<RegistrationState> registration_state{
        RegistrationState::Unregistered};

    // ── Move semantics ──────────────────────────────────────────────
    // `std::atomic<T>` is non-movable by default, so we provide an
    // explicit move that loads the source value and stores it into
    // the destination.  This is only needed during the
    // `RoleHandler(std::vector<Presence>)` construction phase — once
    // the handler's vector is built, no Presence is ever moved.

    Presence() = default;
    Presence(const Presence &) = delete;
    Presence &operator=(const Presence &) = delete;

    Presence(Presence &&other) noexcept
        : hub(std::move(other.hub))
        , channel(std::move(other.channel))
        , role_kind(other.role_kind)
        , slot_spec(std::move(other.slot_spec))
        , fz_spec(std::move(other.fz_spec))
        , connection(other.connection)
    {
        registration_state.store(
            other.registration_state.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    Presence &operator=(Presence &&other) noexcept
    {
        if (this != &other)
        {
            hub        = std::move(other.hub);
            channel    = std::move(other.channel);
            role_kind  = other.role_kind;
            slot_spec  = std::move(other.slot_spec);
            fz_spec    = std::move(other.fz_spec);
            connection = other.connection;
            registration_state.store(
                other.registration_state.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        return *this;
    }
};

}  // namespace pylabhub::scripting
