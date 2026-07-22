#pragma once
/**
 * @file role_handler.hpp
 * @brief Wave-B M3 skeleton — per-role connection + presence manager.
 *
 * `RoleHandler` owns the role's presence list + the deduplicated
 * connection vector + the two O(1) dispatch indexes.  It is the
 * single object that future Wave-B M4 will inject into RoleAPIBase
 * to replace today's hardcoded `broker_channel` pointer + short_tag
 * branching.  Design contract: `docs/archive/transient-2026-06-02/role_host_template_design.md
 * §5.6`.
 *
 * Wave-B M3 ships a structural skeleton — enough surface to write
 * unit tests for dedup logic and index lookup, but NO methods that
 * touch the network.  `start()` / `shutdown()` / `send_class_A` etc.
 * are deferred to Wave-B M4 alongside the call-site swap.
 *
 * Construction contract:
 *   - Caller builds the presence vector (declares the role's
 *     topology: 1 presence for producer/consumer, 2 for processor).
 *   - Caller moves it into the RoleHandler constructor.
 *   - The constructor:
 *       a. dedups by `(hub.broker, hub.broker_pubkey)` →
 *          `connections_` vector (1 entry per unique hub).
 *       b. binds each `Presence::connection` to the matching slot.
 *       c. builds `channel_index_` (`channel_name` → `Presence*`).
 *       d. leaves `band_index_` empty (populated lazily on band_join
 *          in M4+).
 *   - After construction, `presences_` and `connections_` are stable
 *     for the RoleHandler's lifetime — no insertions or removals.
 *     Pointers into either vector are safe to hold until shutdown.
 *
 * Immutability contract (post-construction):
 *   The VECTOR LAYOUT (presences_ and connections_) is frozen.  Pointer
 *   stability follows from this — `connections_.reserve(presences_.size())`
 *   in `build_connections_` guarantees no reallocation during the build
 *   loop, and no later code path mutates either vector.  Future phases
 *   that require dynamic hub failover (re-binding a presence to a new
 *   HubConnection) must re-architect both vectors as stable-storage
 *   containers (`std::list`, intrusive list, or a pinned allocator) —
 *   the current `std::vector` shape cannot grow safely without
 *   invalidating `Presence::connection` pointers.
 *
 *   Individual Presence FIELDS are mutated post-ctor in two scopes:
 *     - `connection`: set during `build_connections_` (M3 ctor).  Never
 *       reassigned afterward.
 *     - `slot_spec`, `fz_spec`, `inbox_meta`: set during the role
 *       host's schema-resolve step (M5+).  Once-write semantics; no
 *       later mutation expected.
 *     - All other fields (`hub`, `channel`, `role_kind`): set by the
 *       caller before passing the vector to the ctor; never mutated
 *       afterward.
 *
 * Thread safety:
 *   The M3-vintage read accessors (`find_presence_for_channel(...)
 *   const`, `presences() const`, `connections() const`,
 *   `presence_count()`, `connection_count()`) are `const` and read
 *   fields that are immutable after construction (per the contract
 *   above) — safe to call from any thread without synchronization.
 *
 *   Audit S1+O4 (2026-05-17) added a non-const overload of
 *   `find_presence_for_channel(channel)` returning `Presence *`
 *   solely to allow the registration FSM mutator
 *   (`presence->registration_state.store(...)`) to be reached
 *   through the standard lookup.  The non-const overload does NOT
 *   add any mutex; per-presence `registration_state` is a
 *   `std::atomic` so concurrent reads/writes are safe by
 *   construction.
 *
 *   Audit R3.3 (2026-05-17) added `mark_connection_disconnected(
 *   HubConnection *)` — a per-connection FSM transition called
 *   from the `on_hub_dead` callback (ctrl thread).  Same atomic
 *   discipline: per-presence atomics, no mutex.
 *
 * Topology stability:
 *   The presence list passed to the ctor is the FULL list of presences
 *   the role will ever hold.  Dynamic add/remove of presences (e.g.,
 *   for hypothetical N-input routers with variable input count) is out
 *   of scope through Wave-B M9; those would require relaxing the
 *   "vectors are stable" invariant above.  Document any such future
 *   feature explicitly when it's introduced — do not silently relax.
 *
 * Invariants verified by L2 tests:
 *   - Distinct hubs → distinct connections (count matches unique
 *     hub identities).
 *   - Same hub → shared connection (single-hub processor with two
 *     presences gets exactly one connection; both
 *     `Presence::connection` pointers reference the same slot).
 *   - `find_presence_for_channel(name)` returns nullptr for unknown
 *     channel; returns the matching presence for a registered one.
 *   - Duplicate-channel presences on one role are forbidden (caller
 *     responsibility — RoleHandler asserts via
 *     `presence_count_for_channel`).
 */

#include "pylabhub_utils_export.h"
#include "utils/hub_connection.hpp" // exposes hub::BrokerRequestComm
#include "utils/json_fwd.hpp"       // nlohmann::json fwd decl
#include "utils/role_presence.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pylabhub::scripting
{

class RoleAPIBase; // fwd — full def in utils/role_api_base.hpp.  Used
                   // only by `start_connections(owner)` to read identity
                   // (uid / name / auth keys) during BRC Config build;
                   // not stored on the handler.

class PYLABHUB_UTILS_EXPORT RoleHandler
{
  public:
    /// Build a role handler from the presence declaration.  Moves the
    /// vector in; dedups it into the `connections_` vector; wires up
    /// the channel index.  See file-header invariants for the post-
    /// construction state.  M3 build-only — no network ops.
    explicit RoleHandler(std::vector<Presence> presences);

    ~RoleHandler() = default;

    RoleHandler(const RoleHandler &) = delete;
    RoleHandler &operator=(const RoleHandler &) = delete;
    RoleHandler(RoleHandler &&) = delete;
    RoleHandler &operator=(RoleHandler &&) = delete;

    // ── Network state lifecycle (Wave-B M4a) ─────────────────────────────────
    //
    // STATE-only.  These methods manipulate the BRC resources owned by
    // each `HubConnection` — they do NOT manage threads.  The role
    // host (which owns the ThreadManager via RoleAPIBase) is the
    // action taker: AFTER `start_connections()` returns, the host
    // iterates `connections()` and spawns one ctrl thread per BRC to
    // drive its poll loop.  BEFORE `stop_connections()` is called,
    // the host MUST signal each BRC's poll loop and join the threads
    // via the ThreadManager Shutdown Contract (HEP-CORE-0031 §4.1).
    //
    // Separation rationale: BrokerRequestComm itself is designed for
    // external thread injection (`run_poll_loop(should_run)` takes the
    // caller's predicate).  The handler mirrors that separation —
    // resources here, execution outside.  This keeps M4-internal tests
    // Pattern-1+ for the handler proper, and keeps thread-spawn policy
    // (master/peer, count visibility, drain ordering) at a single
    // location in the role host integration layer.

    /// Allocate one `BrokerRequestComm` per `HubConnection` slot,
    /// populate its `Config` from `owner`'s identity + auth, and
    /// `connect()` the DEALER socket.  `owner` is borrowed for the
    /// duration of this call only — NOT stored on the handler.
    ///
    /// Returns `true` if every HubConnection connected successfully.
    /// On partial failure, the function returns `false` and releases
    /// any BRCs that were already connected (via `disconnect()` +
    /// `reset()`) so the handler is left in a clean pre-start state.
    ///
    /// Idempotent: calling twice without an intervening
    /// `stop_connections()` returns `false` (no double-allocation).
    [[nodiscard]] bool start_connections(const RoleAPIBase &owner);

    /// Release every `HubConnection`'s BRC: `disconnect()` then reset
    /// the `unique_ptr`.  ASSUMES the caller has already stopped any
    /// threads that were running on these BRCs.  Safe to call when no
    /// start_connections has been issued (no-op).  Idempotent.
    void stop_connections() noexcept;

    /// True iff `start_connections()` has been called and the BRCs
    /// are allocated.  Returns false after `stop_connections()`.
    [[nodiscard]] bool connections_started() const noexcept;

    // ── Read-only accessors (M3 surface) ─────────────────────────────

    /// Number of presences this role declared.  Producer / Consumer
    /// = 1; Processor = 2; future routers ≥ 3.
    [[nodiscard]] std::size_t presence_count() const noexcept { return presences_.size(); }

    /// Number of distinct broker connections after dedup.  Single-hub
    /// processor with 2 presences pointing at the same hub → 1.
    /// Dual-hub processor → 2.  Always ≤ `presence_count()`.
    [[nodiscard]] std::size_t connection_count() const noexcept { return connections_.size(); }

    /// Resolve a channel name to its presence row.  O(1).  Returns
    /// nullptr if the channel is not in this role's presence list.
    /// The returned pointer is stable for the RoleHandler's lifetime.
    [[nodiscard]] const Presence *
    find_presence_for_channel(const std::string &channel) const noexcept;

    /// Non-const overload — same lookup, returns a mutable pointer
    /// so callers can update `Presence::registration_state` (audit
    /// S1+O4, 2026-05-17).  The handler-level vector layout remains
    /// frozen post-construction; only the atomic field on the
    /// Presence can change through this pointer.
    [[nodiscard]] Presence *find_presence_for_channel(const std::string &channel) noexcept;

    /// Number of presences this role holds on a given channel.  Used
    /// to verify the "no duplicate channel within a role" invariant
    /// during testing (M3) and asserted on construction (above).
    [[nodiscard]] std::size_t presence_count_for_channel(const std::string &channel) const noexcept;

    /// Read-only access to the materialised presence vector.  Order
    /// matches construction-time order.  Used by per-presence
    /// heartbeat emission once M4 lands (today the role-side
    /// `on_heartbeat_tick_` enumerates presences from short_tag-driven
    /// branching; that branching collapses to `for (auto &p :
    /// handler_->presences())` in M5+).
    [[nodiscard]] const std::vector<Presence> &presences() const noexcept { return presences_; }

    /// Read-only access to the deduplicated connection vector.  Order
    /// is "first presence to reference this hub wins" — i.e. the
    /// order of unique hubs as encountered in the declared presence
    /// list.
    [[nodiscard]] const std::vector<HubConnection> &connections() const noexcept
    {
        return connections_;
    }

    // ── Routing primitives (Wave-B M4b) ──────────────────────────────────────
    //
    // Per the four-class routing model (HEP-CORE-0033 §18 + design
    // §4), every broker-bound message belongs to exactly one class:
    //
    //   Class A — channel-bound  → route by `channel_name`
    //   Class B — role-bound     → route via any connection (hub-agnostic)
    //   Class C — hub-bound      → route by hub index/identity
    //   Class D — band-bound     → route by `band_name`
    //
    // M4b ships routing primitives that return the BRC for a given
    // routing key.  Callers compose them with BRC's existing typed
    // methods, e.g.:
    //
    //   auto *brc = handler.brc_for_channel(channel);
    //   if (!brc) return error;
    //   brc->register_channel(opts, timeout);
    //
    // No untyped dispatch wrappers — those would duplicate BRC's
    // typed surface (register_channel / send_heartbeat / etc.) for
    // no gain.  Class C ("hub-bound by side") is intentionally NOT
    // wrapped — the "in"/"out" side mapping is role-host topology
    // knowledge.  M4c/d/e migration sites that need Class C dispatch
    // use `handler.connections()[i].brc.get()` directly with `i`
    // chosen by the role host.

    /// Class A — return the `BrokerRequestComm` pointer for the
    /// presence registered on `channel`.  Returns nullptr if the
    /// channel is not in this role's presence list OR if
    /// `start_connections()` has not been called (BRC unallocated).
    /// Disambiguate via `connections_started()`.
    [[nodiscard]] hub::BrokerRequestComm *
    brc_for_channel(const std::string &channel) const noexcept;

    /// Class B — return a `BrokerRequestComm` for role-scope queries
    /// (query_role_presence, query_role_info).  Today: the first
    /// connection in `connections()`.  Role-scope answers from the
    /// broker are not hub-specific — any of our connections suffices.
    /// Returns nullptr if no connections (zero-presence role) or
    /// `start_connections()` not called.
    [[nodiscard]] hub::BrokerRequestComm *brc_for_role() const noexcept;

    /// Class D — return the `BrokerRequestComm` for the presence
    /// that joined `band_name`.  Returns nullptr if no presence has
    /// joined that band (caller must have called `on_band_joined`
    /// for that band first), or if `start_connections()` not called.
    [[nodiscard]] hub::BrokerRequestComm *brc_for_band(const std::string &band_name) const noexcept;

    /// Record that `presence` successfully joined `band_name`.
    /// Populates `band_index_` so subsequent `brc_for_band()` calls
    /// route via `presence`'s connection.  Idempotent — calling with
    /// the same `(band_name, presence)` is harmless.  Calling with
    /// the same `band_name` but a different `presence` overwrites
    /// the prior entry (a role can only be in one role-side
    /// association with a band at a time; the role host decides
    /// which presence "owns" the band).
    void on_band_joined(const std::string &band_name, const Presence *presence) noexcept;

    /// Inverse of `on_band_joined` — remove the band's routing
    /// entry after `band_leave` succeeds.  Safe to call when the
    /// band is not indexed (no-op).
    void on_band_left(const std::string &band_name) noexcept;

    /// Local introspection: does `band_index_` currently have an
    /// entry for @p band_name?  HEP-CORE-0030 amendment 2026-05-19
    /// (S4): role's cached view of its own membership.  See
    /// `RoleAPIBase::is_in_band` for the script-facing wrapper.
    [[nodiscard]] bool is_in_band(const std::string &band_name) const noexcept;

    /// Audit R3.3 (2026-05-17) — transition every Presence whose
    /// `connection` field matches @p dead_conn from
    /// `Registered`/`RegRequestPending` to `Deregistered`.  Called
    /// from the role's `on_hub_dead` callback so the registration
    /// FSM reflects the loss of the broker connection: when ZMTP
    /// declares the peer dead, the broker has already reaped (or
    /// will reap) our presences via heartbeat-timeout; the role's
    /// local FSM should mirror that truth instead of claiming
    /// `Registered` against a dead broker.
    ///
    /// Returns the disconnect summary:
    ///   - `presences_transitioned`: count of Presence rows whose
    ///     `registration_state` went to `Deregistered` (Registered →
    ///     Deregistered, RegRequestPending → Deregistered).
    ///   - `bands_lost`: names of bands whose routing pointed at a
    ///     presence on the dead connection — their `band_index_`
    ///     entries are erased (S4-5 / HEP-CORE-0030 amendment
    ///     2026-05-19: role-side band routing mirrors connection
    ///     liveness).  Caller dispatches an `on_band_lost(band,
    ///     "hub_dead")` notification per name so scripts can react.
    /// Safe to call multiple times — already-Deregistered presences
    /// are skipped; already-erased band entries are no-ops.
    struct DisconnectReap
    {
        std::size_t presences_transitioned{0};
        std::vector<std::string> bands_lost;
    };
    DisconnectReap mark_connection_disconnected(const HubConnection *dead_conn) noexcept;

    /// Extract the originating Presence from an inbound notification
    /// body.  Inspects `body["channel_name"]` first (Class A —
    /// HEP-CORE-0007 wire field on CHANNEL_*_NOTIFY), then
    /// `body["band"]` (Class D — HEP-CORE-0030 §5.1 wire field on
    /// BAND_*_NOTIFY).  Returns nullptr if neither field is present
    /// in the body, or the named channel/band is not in this role's
    /// index (role-scope or hub-scope notification — caller routes
    /// via different logic).  `msg_type` is currently unused but
    /// reserved for future per-msg-type routing rules.
    [[nodiscard]] const Presence *
    find_presence_from_notification(const std::string &msg_type,
                                    const nlohmann::json &body) const noexcept;

  private:
    /// Group presences by `(hub.broker, hub.broker_pubkey)` and build
    /// the `connections_` vector.  Wires each `Presence::connection`
    /// to the matching slot.  Called once by the constructor.
    void build_connections_();

    /// Build the `channel_name` → `Presence*` index.  Called once by
    /// the constructor, after `build_connections_`.
    void build_channel_index_();

    std::vector<Presence> presences_;
    std::vector<HubConnection> connections_;

    /// Class A routing (channel-bound messages) — O(1).
    std::unordered_map<std::string, Presence *> channel_index_;

    /// Class D routing (band-bound messages) — populated lazily on
    /// `band_join` once Wave-B M4 wires the network ops.  Empty in M3.
    std::unordered_map<std::string, Presence *> band_index_;
};

} // namespace pylabhub::scripting
