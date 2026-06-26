/**
 * @file test_hub_state_queries.cpp
 * @brief L2 unit tests for the Layer 2a query helpers in
 *        `utils/hub_state_queries.hpp` (HEP-CORE-0039 Phase A).
 *
 * Pattern 1 — pure in-process tests.  Each helper is a pure
 * function over `ChannelEntry`, `RoleEntry`, `HubStateSnapshot` value
 * types; no `HubState` instance is needed.  Test fixtures construct
 * these structs directly and pass them to the helpers.
 *
 * Pins HEP-CORE-0039 §3.2a contracts:
 *   - `enumerate_live_producers`: only kLive presences included
 *   - `for_each_producer_with_presence`: visitor receives null-able
 *     presence pointer in declaration order
 *   - `for_each_consumer_with_presence`: symmetric
 *   - `for_each_party_identity`: empty-identity skip; kind dispatch
 *   - `find_role_attachments`: multi-attachment vector (role may be
 *     producer on one channel AND consumer on another)
 *   - `is_producer_live`: predicate false on every miss path
 *   - `for_each_presence_matching`: predicate filters; visitor sees
 *     correct PresenceSweepTarget; producers-before-consumers order
 *   - `producer_uids` / `consumer_uids`: declaration order preserved
 */

#include "utils/hub_state.hpp"
#include "utils/hub_state_queries.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using pylabhub::hub::ChannelEntry;
using pylabhub::hub::ChannelObservable;
using pylabhub::hub::ConsumerEntry;
using pylabhub::hub::HubStateSnapshot;
using pylabhub::hub::PartyKind;
using pylabhub::hub::PresenceSweepTarget;
using pylabhub::hub::ProducerEntry;
using pylabhub::hub::RoleAttachment;
using pylabhub::hub::RoleEntry;
using pylabhub::hub::RolePresence;
using pylabhub::hub::RoleState;

using pylabhub::hub::consumer_uids;
using pylabhub::hub::enumerate_live_producers;
using pylabhub::hub::find_role_attachments;
using pylabhub::hub::for_each_consumer_with_presence;
using pylabhub::hub::for_each_party_identity;
using pylabhub::hub::for_each_presence_matching;
using pylabhub::hub::for_each_producer_with_presence;
using pylabhub::hub::is_producer_live;
using pylabhub::hub::producer_uids;

namespace
{

ChannelEntry make_channel(const std::string &name)
{
    ChannelEntry ch;
    ch.name = name;
    return ch;
}

ProducerEntry make_producer(const std::string &uid,
                            const std::string &identity = "")
{
    ProducerEntry p;
    p.role_uid     = uid;
    p.role_name    = uid + "-name";
    p.producer_pid = 1000;
    p.zmq_identity = identity;
    return p;
}

ConsumerEntry make_consumer(const std::string &uid,
                            const std::string &identity = "")
{
    ConsumerEntry c;
    c.role_uid     = uid;
    c.role_name    = uid + "-name";
    c.consumer_pid = 2000;
    c.zmq_identity = identity;
    return c;
}

RolePresence make_presence(const std::string &channel,
                           const std::string &role_type,
                           RoleState state                = RoleState::Connected,
                           bool first_heartbeat_seen      = true)
{
    RolePresence p;
    p.channel              = channel;
    p.role_type            = role_type;
    p.state                = state;
    p.first_heartbeat_seen = first_heartbeat_seen;
    return p;
}

RoleEntry make_role(const std::string &uid,
                    std::vector<RolePresence> presences = {})
{
    RoleEntry r;
    r.uid       = uid;
    r.name      = uid + "-name";
    r.short_tag  = "test";
    r.presences = std::move(presences);
    return r;
}

using RolesMap = std::unordered_map<std::string, RoleEntry>;

} // namespace

// ── enumerate_live_producers ────────────────────────────────────────────────

TEST(QueryEnumerateLiveProducers, EmptyChannel_ReturnsEmpty)
{
    ChannelEntry ch = make_channel("ch1");
    RolesMap     roles;
    auto live = enumerate_live_producers(ch, roles);
    EXPECT_TRUE(live.empty());
}

TEST(QueryEnumerateLiveProducers, ProducerWithNoRoleRow_NotLive)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a.uid"));
    RolesMap roles;  // role row missing
    auto     live = enumerate_live_producers(ch, roles);
    EXPECT_TRUE(live.empty());
}

TEST(QueryEnumerateLiveProducers, ProducerConnectedNoHeartbeat_NotLive)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a.uid"));
    RolesMap roles;
    roles["prod.a.uid"] = make_role(
        "prod.a.uid",
        {make_presence("ch1", "producer", RoleState::Connected,
                        /*first_heartbeat_seen=*/false)});
    auto live = enumerate_live_producers(ch, roles);
    EXPECT_TRUE(live.empty())
        << "kRegistering (Connected without first_heartbeat) is NOT kLive";
}

TEST(QueryEnumerateLiveProducers, ProducerPending_NotLive)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a.uid"));
    RolesMap roles;
    roles["prod.a.uid"] = make_role(
        "prod.a.uid",
        {make_presence("ch1", "producer", RoleState::Pending, true)});
    auto live = enumerate_live_producers(ch, roles);
    EXPECT_TRUE(live.empty());
}

TEST(QueryEnumerateLiveProducers, ProducerLive_Included)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a.uid"));
    RolesMap roles;
    roles["prod.a.uid"] = make_role(
        "prod.a.uid",
        {make_presence("ch1", "producer", RoleState::Connected, true)});
    auto live = enumerate_live_producers(ch, roles);
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0]->role_uid, "prod.a.uid");
}

TEST(QueryEnumerateLiveProducers, MultipleProducers_OrderPreserved)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a.uid"));
    ch.producers.push_back(make_producer("prod.b.uid"));  // not live
    ch.producers.push_back(make_producer("prod.c.uid"));
    RolesMap roles;
    roles["prod.a.uid"] = make_role(
        "prod.a.uid",
        {make_presence("ch1", "producer", RoleState::Connected, true)});
    roles["prod.b.uid"] = make_role(
        "prod.b.uid",
        {make_presence("ch1", "producer", RoleState::Pending, true)});
    roles["prod.c.uid"] = make_role(
        "prod.c.uid",
        {make_presence("ch1", "producer", RoleState::Connected, true)});

    auto live = enumerate_live_producers(ch, roles);
    ASSERT_EQ(live.size(), 2u);
    EXPECT_EQ(live[0]->role_uid, "prod.a.uid");
    EXPECT_EQ(live[1]->role_uid, "prod.c.uid")
        << "declaration order preserved; non-live entries skipped";
}

// ── for_each_producer_with_presence ────────────────────────────────────────

TEST(QueryForEachProducerWithPresence, EmptyChannel_VisitorNotCalled)
{
    ChannelEntry ch = make_channel("ch1");
    RolesMap     roles;
    int          calls = 0;
    for_each_producer_with_presence(
        ch, roles, [&](const ProducerEntry &, const RolePresence *) {
            ++calls;
        });
    EXPECT_EQ(calls, 0);
}

TEST(QueryForEachProducerWithPresence, ProducerWithNoRoleRow_NullPresence)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a.uid"));
    RolesMap roles;
    int      calls = 0;
    for_each_producer_with_presence(
        ch, roles, [&](const ProducerEntry &prod, const RolePresence *p) {
            ++calls;
            EXPECT_EQ(prod.role_uid, "prod.a.uid");
            EXPECT_EQ(p, nullptr)
                << "missing role row → null presence (legitimate state)";
        });
    EXPECT_EQ(calls, 1);
}

TEST(QueryForEachProducerWithPresence, IterationOrderMatchesDeclaration)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a"));
    ch.producers.push_back(make_producer("prod.b"));
    ch.producers.push_back(make_producer("prod.c"));
    RolesMap roles;

    std::vector<std::string> seen;
    for_each_producer_with_presence(
        ch, roles, [&](const ProducerEntry &prod, const RolePresence *) {
            seen.push_back(prod.role_uid);
        });
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], "prod.a");
    EXPECT_EQ(seen[1], "prod.b");
    EXPECT_EQ(seen[2], "prod.c");
}

// ── for_each_consumer_with_presence ────────────────────────────────────────

TEST(QueryForEachConsumerWithPresence, ConsumerWithLivePresence_Visited)
{
    ChannelEntry ch = make_channel("ch1");
    ch.consumers.push_back(make_consumer("cons.a"));
    RolesMap roles;
    roles["cons.a"] = make_role(
        "cons.a",
        {make_presence("ch1", "consumer", RoleState::Connected, true)});

    int calls = 0;
    for_each_consumer_with_presence(
        ch, roles, [&](const ConsumerEntry &c, const RolePresence *p) {
            ++calls;
            EXPECT_EQ(c.role_uid, "cons.a");
            ASSERT_NE(p, nullptr);
            EXPECT_EQ(p->role_type, "consumer");
        });
    EXPECT_EQ(calls, 1);
}

// ── for_each_party_identity ────────────────────────────────────────────────

TEST(QueryForEachPartyIdentity, EmptyIdentitySkipped)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("p.a", ""));      // skipped
    ch.producers.push_back(make_producer("p.b", "ident-b"));
    ch.producers.push_back(make_producer("p.c", ""));      // skipped
    ch.producers.push_back(make_producer("p.d", "ident-d"));

    std::vector<std::string> seen;
    for_each_party_identity(
        ch, PartyKind::Producer,
        [&](std::string_view id, std::string_view) {
            seen.emplace_back(id);
        });
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], "ident-b");
    EXPECT_EQ(seen[1], "ident-d");
}

TEST(QueryForEachPartyIdentity, KindDispatchToConsumers)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("p.a", "p-ident"));
    ch.consumers.push_back(make_consumer("c.a", "c-ident"));

    std::vector<std::string> as_consumer;
    for_each_party_identity(
        ch, PartyKind::Consumer,
        [&](std::string_view id, std::string_view uid) {
            as_consumer.emplace_back(std::string(id) + ":" +
                                      std::string(uid));
        });
    ASSERT_EQ(as_consumer.size(), 1u);
    EXPECT_EQ(as_consumer[0], "c-ident:c.a")
        << "Consumer dispatch must NOT visit producers";
}

// ── find_role_attachments ──────────────────────────────────────────────────

TEST(QueryFindRoleAttachments, EmptySnapshot_ReturnsEmpty)
{
    HubStateSnapshot snap;
    auto             a = find_role_attachments(snap, "any.uid");
    EXPECT_TRUE(a.empty());
}

TEST(QueryFindRoleAttachments, RoleProducerOnly)
{
    HubStateSnapshot snap;
    ChannelEntry    &ch = snap.channels["ch1"] = make_channel("ch1");
    ch.producers.push_back(make_producer("role.x"));

    auto a = find_role_attachments(snap, "role.x");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].channel, "ch1");
    EXPECT_EQ(a[0].role_type, "producer");
    EXPECT_NE(a[0].producer, nullptr);
    EXPECT_EQ(a[0].consumer, nullptr);
}

TEST(QueryFindRoleAttachments, RoleProducerAndConsumer_Multichannel)
{
    HubStateSnapshot snap;
    ChannelEntry    &c1 = snap.channels["ch1"] = make_channel("ch1");
    c1.producers.push_back(make_producer("role.x"));
    ChannelEntry    &c2 = snap.channels["ch2"] = make_channel("ch2");
    c2.consumers.push_back(make_consumer("role.x"));

    auto a = find_role_attachments(snap, "role.x");
    ASSERT_EQ(a.size(), 2u);
    // Order is unordered_map iteration order — collect into a sorted
    // set for deterministic assertion.
    std::vector<std::string> tags;
    for (const auto &att : a)
        tags.push_back(att.channel + ":" + att.role_type);
    std::sort(tags.begin(), tags.end());
    EXPECT_EQ(tags[0], "ch1:producer");
    EXPECT_EQ(tags[1], "ch2:consumer");
}

TEST(QueryFindRoleAttachments, RoleAsBothOnSameChannel)
{
    HubStateSnapshot snap;
    ChannelEntry    &c1 = snap.channels["ch1"] = make_channel("ch1");
    c1.producers.push_back(make_producer("proc.role"));
    c1.consumers.push_back(make_consumer("proc.role"));

    auto a = find_role_attachments(snap, "proc.role");
    ASSERT_EQ(a.size(), 2u)
        << "Processor pattern: role attached as both producer + consumer "
           "on same channel produces TWO RoleAttachment rows";
    EXPECT_EQ(a[0].channel, "ch1");
    EXPECT_EQ(a[1].channel, "ch1");
    EXPECT_NE(a[0].role_type, a[1].role_type);
}

// ── is_producer_live ───────────────────────────────────────────────────────

TEST(QueryIsProducerLive, NotAProducerOfChannel_False)
{
    ChannelEntry ch = make_channel("ch1");
    RolesMap     roles;
    EXPECT_FALSE(is_producer_live(ch, "any.uid", roles));
}

TEST(QueryIsProducerLive, ProducerButNoRoleRow_False)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a"));
    RolesMap roles;
    EXPECT_FALSE(is_producer_live(ch, "prod.a", roles));
}

TEST(QueryIsProducerLive, ProducerPending_False)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a"));
    RolesMap roles;
    roles["prod.a"] = make_role(
        "prod.a",
        {make_presence("ch1", "producer", RoleState::Pending, true)});
    EXPECT_FALSE(is_producer_live(ch, "prod.a", roles));
}

TEST(QueryIsProducerLive, ProducerLive_True)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("prod.a"));
    RolesMap roles;
    roles["prod.a"] = make_role(
        "prod.a",
        {make_presence("ch1", "producer", RoleState::Connected, true)});
    EXPECT_TRUE(is_producer_live(ch, "prod.a", roles));
}

// ── for_each_presence_matching (sweep visitor) ─────────────────────────────

TEST(QueryForEachPresenceMatching, PredicateFiltersPerPresence)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("p.live"));
    ch.producers.push_back(make_producer("p.pend"));
    ch.consumers.push_back(make_consumer("c.live"));
    ch.consumers.push_back(make_consumer("c.pend"));

    RolesMap roles;
    roles["p.live"] = make_role(
        "p.live",
        {make_presence("ch1", "producer", RoleState::Connected, true)});
    roles["p.pend"] = make_role(
        "p.pend",
        {make_presence("ch1", "producer", RoleState::Pending, true)});
    roles["c.live"] = make_role(
        "c.live",
        {make_presence("ch1", "consumer", RoleState::Connected, true)});
    roles["c.pend"] = make_role(
        "c.pend",
        {make_presence("ch1", "consumer", RoleState::Pending, true)});

    // Filter: only Pending presences.
    std::vector<std::string> hit;
    for_each_presence_matching(
        ch, roles,
        [](const RolePresence &p) { return p.state == RoleState::Pending; },
        [&](const PresenceSweepTarget &t) {
            const std::string uid =
                (t.party == PartyKind::Producer ? t.producer->role_uid
                                                : t.consumer->role_uid);
            hit.push_back(std::string(t.channel) + ":" + uid);
        });
    ASSERT_EQ(hit.size(), 2u);
    EXPECT_EQ(hit[0], "ch1:p.pend")
        << "producers visited before consumers";
    EXPECT_EQ(hit[1], "ch1:c.pend");
}

TEST(QueryForEachPresenceMatching, NoMatch_VisitorNotCalled)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("p.live"));
    RolesMap roles;
    roles["p.live"] = make_role(
        "p.live",
        {make_presence("ch1", "producer", RoleState::Connected, true)});

    int calls = 0;
    for_each_presence_matching(
        ch, roles,
        [](const RolePresence &p) { return p.state == RoleState::Pending; },
        [&](const PresenceSweepTarget &) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST(QueryForEachPresenceMatching, TargetCarriesCopiedPresence)
{
    // The sweep visitor must see a VALUE copy of the presence,
    // not a pointer.  Pins the "read-only" contract (HEP-0039 §3.2a).
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("p.a"));
    RolesMap roles;
    roles["p.a"] = make_role(
        "p.a",
        {make_presence("ch1", "producer", RoleState::Pending, true)});

    bool visited = false;
    for_each_presence_matching(
        ch, roles, [](const RolePresence &) { return true; },
        [&](const PresenceSweepTarget &t) {
            visited = true;
            EXPECT_EQ(t.presence.state, RoleState::Pending);
            EXPECT_EQ(t.presence.channel, "ch1");
            EXPECT_EQ(t.presence.role_type, "producer");
            EXPECT_EQ(t.channel, "ch1");
            EXPECT_EQ(t.party, PartyKind::Producer);
            EXPECT_NE(t.producer, nullptr);
            EXPECT_EQ(t.consumer, nullptr);
            // channel_entry points to the parent ChannelEntry — needed
            // by callers that capture pre_drop before applying a
            // channel-erasing mutator.
            ASSERT_NE(t.channel_entry, nullptr);
            EXPECT_EQ(t.channel_entry, &ch);
            EXPECT_EQ(t.channel_entry->name, "ch1");
        });
    EXPECT_TRUE(visited);
}

TEST(QueryForEachPresenceMatching, ConsumerBranch_TargetFullyPopulated)
{
    // Symmetric to TargetCarriesCopiedPresence but for the consumer
    // branch.  A regression that drops the `t.channel_entry = &ch`
    // (or any other field) assignment from the consumer branch of
    // `for_each_presence_matching` would not be caught by the
    // producer-side test — Step B's Pass-2 consumer apply phase
    // dereferences `channel_entry` for the `pre_drop` capture and
    // would null-deref crash without this pin.
    ChannelEntry ch = make_channel("ch2");
    ch.consumers.push_back(make_consumer("c.a"));
    RolesMap roles;
    roles["c.a"] = make_role(
        "c.a",
        {make_presence("ch2", "consumer", RoleState::Pending, true)});

    bool visited = false;
    for_each_presence_matching(
        ch, roles, [](const RolePresence &) { return true; },
        [&](const PresenceSweepTarget &t) {
            visited = true;
            EXPECT_EQ(t.presence.state, RoleState::Pending);
            EXPECT_EQ(t.presence.channel, "ch2");
            EXPECT_EQ(t.presence.role_type, "consumer");
            EXPECT_EQ(t.channel, "ch2");
            EXPECT_EQ(t.party, PartyKind::Consumer);
            EXPECT_EQ(t.producer, nullptr);
            EXPECT_NE(t.consumer, nullptr);
            ASSERT_NE(t.channel_entry, nullptr);
            EXPECT_EQ(t.channel_entry, &ch);
            EXPECT_EQ(t.channel_entry->name, "ch2");
        });
    EXPECT_TRUE(visited);
}

// ── producer_uids / consumer_uids ──────────────────────────────────────────

TEST(QueryUidExtractors, EmptyChannel_EmptyVectors)
{
    ChannelEntry ch = make_channel("ch1");
    EXPECT_TRUE(producer_uids(ch).empty());
    EXPECT_TRUE(consumer_uids(ch).empty());
}

TEST(QueryUidExtractors, DeclarationOrderPreserved)
{
    ChannelEntry ch = make_channel("ch1");
    ch.producers.push_back(make_producer("p.first"));
    ch.producers.push_back(make_producer("p.second"));
    ch.producers.push_back(make_producer("p.third"));
    ch.consumers.push_back(make_consumer("c.first"));
    ch.consumers.push_back(make_consumer("c.second"));

    auto pu = producer_uids(ch);
    ASSERT_EQ(pu.size(), 3u);
    EXPECT_EQ(pu[0], "p.first");
    EXPECT_EQ(pu[1], "p.second");
    EXPECT_EQ(pu[2], "p.third");

    auto cu = consumer_uids(ch);
    ASSERT_EQ(cu.size(), 2u);
    EXPECT_EQ(cu[0], "c.first");
    EXPECT_EQ(cu[1], "c.second");
}

// ── HubStateSnapshot metadata (HEP-0039 §3.1) ──────────────────────────────

TEST(HubStateSnapshotMetadata, DefaultConstructed_SeqIsZero)
{
    HubStateSnapshot snap;
    EXPECT_EQ(snap.snapshot_seq, 0u)
        << "seq == 0 is reserved for default-constructed snapshots; "
           "HubState::snapshot() must never return seq == 0";
    EXPECT_TRUE(snap.hub_uid.empty());
}

TEST(HubStateSnapshotMetadata, HubStateSnapshot_PopulatesMetadata)
{
    pylabhub::hub::HubState s;
    s.set_hub_uid("hub.test.uid12345678");

    auto snap1 = s.snapshot();
    EXPECT_EQ(snap1.hub_uid, "hub.test.uid12345678");
    EXPECT_EQ(snap1.snapshot_seq, 1u)
        << "First live snapshot has seq == 1 (HEP-0039 §3.1)";

    auto snap2 = s.snapshot();
    EXPECT_EQ(snap2.snapshot_seq, 2u)
        << "Counter monotonically increments per call";

    // captured_mono advances between captures.
    EXPECT_GE(snap2.captured_mono, snap1.captured_mono);
}
