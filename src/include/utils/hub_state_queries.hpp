#pragma once
/**
 * @file hub_state_queries.hpp
 * @brief Layer 2a query primitives for HubState — typed helpers that
 *        replace inline joins across `broker_service.cpp` and
 *        `hub_state.cpp`.
 *
 * Canonical home for the new query helpers introduced by
 * HEP-CORE-0039 (Hub State Query Layer).  Each helper here returns
 * a domain type (`std::optional<T>`, `const T *`,
 * `std::vector<const T *>`, or invokes a visitor) — distinct from
 * Layer 2b helpers (in a separate file) which return
 * `nlohmann::json` for script consumers.
 *
 * **`RolesMap` template contract** (HEP-0039 §3.2a): the templated
 * primitives accept any type satisfying the following duck-typed
 * shape:
 *   - `.find(string-like)` returns an iterator-like type
 *   - the iterator is comparable to `.end()`
 *   - `it->second` exposes
 *     `.find_presence(channel, role_type) const noexcept
 *      -> const RolePresence *`
 * Concrete satisfiers: `HubStateSnapshot::roles` (value-typed) and
 * `HubState::Impl::roles` (live, internal).
 *
 * Mutators are out of scope (governed by HEP-CORE-0033 and the Core
 * Structure Change Protocol).  Wire serializers are out of scope
 * (`hub_state_json.hpp`).
 *
 * @see HEP-CORE-0039 §3.2a
 */

#include "utils/hub_state.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pylabhub::hub
{

// ─── Tag types + result structs ─────────────────────────────────────────────

/// Distinguishes which side of a channel a helper is walking.
/// Used by `for_each_party_identity` and `PresenceSweepTarget`.
enum class PartyKind { Producer, Consumer };

/// Result of `find_role_attachments` — one entry per `(channel,
/// role_type)` row the role is attached to.  Exactly one of
/// `producer` / `consumer` is non-null (the other is the side the
/// role is NOT attached as on this channel).
struct RoleAttachment
{
    std::string           channel;
    std::string           role_type;            ///< "producer" | "consumer"
    const ProducerEntry  *producer{nullptr};
    const ConsumerEntry  *consumer{nullptr};
};

/// Payload passed to the `for_each_presence_matching` visitor.
/// **Read-only contract per HEP-0039 §3.2a two-phase pattern**:
/// the visitor MUST NOT mutate hub state directly.  Decisions
/// should be recorded in a caller-owned container and applied via
/// per-mutator capability ops (see lock-discipline note below).
///
/// `presence` is a value copy of the matching `RolePresence`;
/// `producer` / `consumer` point into the channel's vector (one is
/// null per `party`); `channel_entry` points to the parent
/// `ChannelEntry` (always non-null inside the visitor; useful when
/// the apply phase needs to capture a `pre_drop` snapshot of the
/// channel before the mutator erases it from live state).
struct PresenceSweepTarget
{
    PartyKind             party;
    const ProducerEntry  *producer{nullptr};
    const ConsumerEntry  *consumer{nullptr};
    RolePresence          presence;             ///< copied
    std::string           channel;
    const ChannelEntry   *channel_entry{nullptr};
};

// ─── Helper: resolve a producer's presence row ──────────────────────────────

namespace detail
{

template <typename RolesMap>
inline const RolePresence *
resolve_producer_presence(const std::string &channel_name,
                           const std::string &role_uid,
                           const RolesMap    &roles) noexcept
{
    auto it = roles.find(role_uid);
    if (it == roles.end()) return nullptr;
    return it->second.find_presence(channel_name, "producer");
}

template <typename RolesMap>
inline const RolePresence *
resolve_consumer_presence(const std::string &channel_name,
                           const std::string &role_uid,
                           const RolesMap    &roles) noexcept
{
    auto it = roles.find(role_uid);
    if (it == roles.end()) return nullptr;
    return it->second.find_presence(channel_name, "consumer");
}

} // namespace detail

// ─── for_each_producer_with_presence / consumer ─────────────────────────────

/// Iterate every producer of @p ch in declaration order.  For each,
/// look up its presence row in @p roles and invoke
/// `fn(producer, presence_or_null)`.  The presence pointer is null
/// when the role row is missing or has no producer-presence for
/// this channel — a legitimate state during late presence drop and
/// NOT to be treated as an error.
///
/// Visitor signature: `void(const ProducerEntry &, const RolePresence *)`.
template <typename RolesMap, typename Fn>
inline void
for_each_producer_with_presence(const ChannelEntry &ch,
                                 const RolesMap     &roles,
                                 Fn                 fn)
{
    for (const auto &prod : ch.producers)
    {
        const RolePresence *p =
            detail::resolve_producer_presence(ch.name, prod.role_uid, roles);
        fn(prod, p);
    }
}

/// Symmetric consumer-side walker.  Visitor signature:
/// `void(const ConsumerEntry &, const RolePresence *)`.
template <typename RolesMap, typename Fn>
inline void
for_each_consumer_with_presence(const ChannelEntry &ch,
                                 const RolesMap     &roles,
                                 Fn                 fn)
{
    for (const auto &cons : ch.consumers)
    {
        const RolePresence *p =
            detail::resolve_consumer_presence(ch.name, cons.role_uid, roles);
        fn(cons, p);
    }
}

// ─── enumerate_live_producers ───────────────────────────────────────────────

/// Return pointers into @p ch's `producers[]` vector for every
/// producer whose `(channel, producer)` presence row is currently
/// `Connected` AND `first_heartbeat_seen` (i.e., kLive per
/// HEP-0023).  Order matches `ch.producers[]` declaration order.
/// Returns an empty vector if no producer is live.
///
/// This is the canonical helper for "to whom should I push a
/// per-producer broker message?" — Phase D's `CHANNEL_AUTH_UPDATE`
/// emitter is one consumer.
template <typename RolesMap>
inline std::vector<const ProducerEntry *>
enumerate_live_producers(const ChannelEntry &ch,
                          const RolesMap     &roles)
{
    std::vector<const ProducerEntry *> out;
    out.reserve(ch.producers.size());
    for_each_producer_with_presence(
        ch, roles,
        [&](const ProducerEntry &prod, const RolePresence *p) {
            if (p == nullptr) return;
            if (ch.observe(p) == ChannelObservable::kLive)
                out.push_back(&prod);
        });
    return out;
}

// ─── is_producer_live (predicate) ───────────────────────────────────────────

/// True iff @p role_uid is a producer of @p ch AND its presence
/// row is currently kLive.  False if the producer is not on the
/// channel, its role is missing, its presence row is missing, or
/// its observable is sub-Live.
template <typename RolesMap>
inline bool
is_producer_live(const ChannelEntry &ch,
                  const std::string &role_uid,
                  const RolesMap    &roles) noexcept
{
    if (ch.find_producer(role_uid) == nullptr) return false;
    const RolePresence *p =
        detail::resolve_producer_presence(ch.name, role_uid, roles);
    return p != nullptr && ch.observe(p) == ChannelObservable::kLive;
}

// ─── for_each_party_identity ───────────────────────────────────────────────

/// Visit every member of @p ch (producers or consumers per @p kind)
/// whose `zmq_identity` is non-empty.  The empty-identity skip is
/// the canonical fan-out guard — a member without a routing key
/// cannot receive a NOTIFY and is silently skipped.
///
/// Visitor signature:
/// `void(std::string_view zmq_identity, std::string_view role_uid)`.
template <typename Fn>
inline void
for_each_party_identity(const ChannelEntry &ch,
                         PartyKind          kind,
                         Fn                fn)
{
    if (kind == PartyKind::Producer)
    {
        for (const auto &prod : ch.producers)
        {
            if (prod.zmq_identity.empty()) continue;
            fn(std::string_view{prod.zmq_identity},
               std::string_view{prod.role_uid});
        }
    }
    else
    {
        for (const auto &cons : ch.consumers)
        {
            if (cons.zmq_identity.empty()) continue;
            fn(std::string_view{cons.zmq_identity},
               std::string_view{cons.role_uid});
        }
    }
}

// ─── find_role_attachments ──────────────────────────────────────────────────

/// Find every `(channel, role_type)` attachment for a role across
/// the hub.  A role may appear as producer on one channel AND
/// consumer on another (HEP-0023 §2.1 + HEP-0033 §19); the result
/// is a vector of every match.  Empty vector ⇒ role is not
/// attached to any channel as producer or consumer.
///
/// Visit order: channels iterated in `snap.channels` order (which
/// is `unordered_map` order — NOT guaranteed sorted); for each
/// channel producers are checked before consumers.
inline std::vector<RoleAttachment>
find_role_attachments(const HubStateSnapshot &snap,
                       const std::string     &uid)
{
    std::vector<RoleAttachment> out;
    for (const auto &[name, ch] : snap.channels)
    {
        if (const ProducerEntry *prod = ch.find_producer(uid))
        {
            out.push_back(
                RoleAttachment{name, "producer", prod, nullptr});
        }
        if (const ConsumerEntry *cons = ch.find_consumer(uid))
        {
            out.push_back(
                RoleAttachment{name, "consumer", nullptr, cons});
        }
    }
    return out;
}

// ─── for_each_presence_matching (sweep visitor) ────────────────────────────

/// Visit every (producer or consumer) presence on @p ch whose
/// presence-row matches @p pred.  Walks producers first, then
/// consumers.  Predicate signature: `bool(const RolePresence &)`.
/// Visitor signature: `void(const PresenceSweepTarget &)`.
///
/// **Two-phase contract (HEP-0039 §3.2a).**  The visitor MUST NOT
/// mutate hub state.  Use this helper to collect decisions into a
/// caller-owned container; apply them in a separate pass.
///
/// **Lock discipline (per HEP-0039 §2 + §3.2a).**  Two valid modes:
///
///   (a) Snapshot mode (the canonical mode for this helper).  The
///       caller holds a `HubStateSnapshot` (value copy taken under
///       `shared_lock` then released).  The visit phase walks the
///       snapshot with NO lock held; pointers in
///       `PresenceSweepTarget` are stable for the snapshot's
///       lifetime.  The apply phase calls per-mutator capability
///       ops on the live `HubState` — each op takes its own writer
///       lock; the caller does NOT hold a writer lock across the
///       apply phase.  This is what
///       `BrokerServiceImpl::check_heartbeat_timeouts` does today.
///
///   (b) Writer-lock mode.  The caller holds the writer lock for
///       both visit and apply, calling mutators that DON'T take the
///       lock themselves.  Rare; not the heartbeat-timeout shape.
///
/// **Cross-pass dependencies require two snapshots + two passes.**
/// The heartbeat-timeout sweep is the canonical example: Pass-1
/// transitions Connected→Pending and stamps a fresh `state_since`;
/// Pass-2 transitions Pending→Disconnected only when
/// `now - state_since >= pending_timeout`.  Because Pass-2 must
/// observe Pass-1's fresh `state_since` stamps (to EXCLUDE
/// just-demoted presences from same-sweep termination), the caller
/// must (1) take Snapshot-1, (2) run `for_each_presence_matching`
/// with the Pass-1 predicate + collect decisions, (3) drain the
/// decisions via the Pass-1 mutators, (4) take Snapshot-2 — fresh,
/// reflecting Pass-1's mutations, (5) run
/// `for_each_presence_matching` again with the Pass-2 predicate +
/// collect, (6) drain via Pass-2 mutators.  A single helper call
/// with a "Pass-1-or-Pass-2" predicate over ONE snapshot is wrong:
/// it would consider just-demoted presences for termination in the
/// same tick.  See HEP-CORE-0039 §6 "Two-passes-with-cross-pass-
/// dependency note" for the canonical statement.
template <typename RolesMap, typename Pred, typename Fn>
inline void
for_each_presence_matching(const ChannelEntry &ch,
                            const RolesMap     &roles,
                            Pred               pred,
                            Fn                 fn)
{
    for (const auto &prod : ch.producers)
    {
        const RolePresence *p =
            detail::resolve_producer_presence(ch.name, prod.role_uid, roles);
        if (p == nullptr || !pred(*p)) continue;
        PresenceSweepTarget t;
        t.party         = PartyKind::Producer;
        t.producer      = &prod;
        t.consumer      = nullptr;
        t.presence      = *p;
        t.channel       = ch.name;
        t.channel_entry = &ch;
        fn(t);
    }
    for (const auto &cons : ch.consumers)
    {
        const RolePresence *p =
            detail::resolve_consumer_presence(ch.name, cons.role_uid, roles);
        if (p == nullptr || !pred(*p)) continue;
        PresenceSweepTarget t;
        t.party         = PartyKind::Consumer;
        t.producer      = nullptr;
        t.consumer      = &cons;
        t.presence      = *p;
        t.channel       = ch.name;
        t.channel_entry = &ch;
        fn(t);
    }
}

// ─── producer_uids / consumer_uids ──────────────────────────────────────────

/// Extract producer `role_uid`s from @p ch in declaration order.
inline std::vector<std::string>
producer_uids(const ChannelEntry &ch)
{
    std::vector<std::string> out;
    out.reserve(ch.producers.size());
    for (const auto &prod : ch.producers) out.push_back(prod.role_uid);
    return out;
}

/// Extract consumer `role_uid`s from @p ch in declaration order.
inline std::vector<std::string>
consumer_uids(const ChannelEntry &ch)
{
    std::vector<std::string> out;
    out.reserve(ch.consumers.size());
    for (const auto &cons : ch.consumers) out.push_back(cons.role_uid);
    return out;
}

} // namespace pylabhub::hub
