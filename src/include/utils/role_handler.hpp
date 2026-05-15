#pragma once
/**
 * @file role_handler.hpp
 * @brief Wave-B M3 skeleton — per-role connection + presence manager.
 *
 * `RoleHandler` owns the role's presence list + the deduplicated
 * connection vector + the two O(1) dispatch indexes.  It is the
 * single object that future Wave-B M4 will inject into RoleAPIBase
 * to replace today's hardcoded `broker_channel` pointer + role_tag
 * branching.  Design contract: `role_host_template_design.md §5.6`.
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
 *   All M3 accessors are `const` and read fields that are immutable
 *   after construction (per the contract above).  RoleHandler is safe
 *   to call from any thread without synchronization, AS LONG AS the
 *   immutability contract holds.  Wave-B M4 will add `start()` /
 *   `shutdown()` + dispatch ops that touch the network and spawn ctrl
 *   threads — at that point M4 must document which methods are safe
 *   from which threads.
 *
 * Topology stability:
 *   The presence list passed to the ctor is the FULL list of presences
 *   the role will ever hold.  Dynamic add/remove of presences (e.g.,
 *   for hypothetical N-input routers with variable input count) is out
 *   of scope through Wave-B M9; those would require relaxing the
 *   "vectors are stable" invariant above.  Document any such future
 *   feature explicitly when it's introduced — do not silently relax.
 *
 * Wave-B M3 invariants verified by L2 tests:
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
#include "utils/hub_connection.hpp"
#include "utils/role_presence.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pylabhub::scripting
{

class PYLABHUB_UTILS_EXPORT RoleHandler
{
  public:
    /// Build a role handler from the presence declaration.  Moves the
    /// vector in; dedups it into the `connections_` vector; wires up
    /// the channel index.  See file-header invariants for the post-
    /// construction state.  M3 build-only — no network ops.
    explicit RoleHandler(std::vector<Presence> presences);

    ~RoleHandler() = default;

    RoleHandler(const RoleHandler &)            = delete;
    RoleHandler &operator=(const RoleHandler &) = delete;
    RoleHandler(RoleHandler &&)                 = delete;
    RoleHandler &operator=(RoleHandler &&)      = delete;

    // ── Read-only accessors (M3 surface) ─────────────────────────────

    /// Number of presences this role declared.  Producer / Consumer
    /// = 1; Processor = 2; future routers ≥ 3.
    [[nodiscard]] std::size_t presence_count() const noexcept
    {
        return presences_.size();
    }

    /// Number of distinct broker connections after dedup.  Single-hub
    /// processor with 2 presences pointing at the same hub → 1.
    /// Dual-hub processor → 2.  Always ≤ `presence_count()`.
    [[nodiscard]] std::size_t connection_count() const noexcept
    {
        return connections_.size();
    }

    /// Resolve a channel name to its presence row.  O(1).  Returns
    /// nullptr if the channel is not in this role's presence list.
    /// The returned pointer is stable for the RoleHandler's lifetime.
    [[nodiscard]] const Presence *
    find_presence_for_channel(const std::string &channel) const noexcept;

    /// Number of presences this role holds on a given channel.  Used
    /// to verify the "no duplicate channel within a role" invariant
    /// during testing (M3) and asserted on construction (above).
    [[nodiscard]] std::size_t
    presence_count_for_channel(const std::string &channel) const noexcept;

    /// Read-only access to the materialised presence vector.  Order
    /// matches construction-time order.  Used by per-presence
    /// heartbeat emission once M4 lands (today the role-side
    /// `on_heartbeat_tick_` enumerates presences from role_tag-driven
    /// branching; that branching collapses to `for (auto &p :
    /// handler_->presences())` in M5+).
    [[nodiscard]] const std::vector<Presence> &presences() const noexcept
    {
        return presences_;
    }

    /// Read-only access to the deduplicated connection vector.  Order
    /// is "first presence to reference this hub wins" — i.e. the
    /// order of unique hubs as encountered in the declared presence
    /// list.
    [[nodiscard]] const std::vector<HubConnection> &connections() const noexcept
    {
        return connections_;
    }

    // Network-touching surface (start/shutdown + four-class dispatch +
    // band_join lazy index population) arrives in Wave-B M4 per
    // `docs/tech_draft/role_host_template_design.md §5.6` (sketch) +
    // §14.2 (M4 row).  See those sections for the authoritative list;
    // duplicating the signatures here would rot the moment M4 ships.

  private:
    /// Group presences by `(hub.broker, hub.broker_pubkey)` and build
    /// the `connections_` vector.  Wires each `Presence::connection`
    /// to the matching slot.  Called once by the constructor.
    void build_connections_();

    /// Build the `channel_name` → `Presence*` index.  Called once by
    /// the constructor, after `build_connections_`.
    void build_channel_index_();

    std::vector<Presence>      presences_;
    std::vector<HubConnection> connections_;

    /// Class A routing (channel-bound messages) — O(1).
    std::unordered_map<std::string, Presence *> channel_index_;

    /// Class D routing (band-bound messages) — populated lazily on
    /// `band_join` once Wave-B M4 wires the network ops.  Empty in M3.
    std::unordered_map<std::string, Presence *> band_index_;
};

}  // namespace pylabhub::scripting
