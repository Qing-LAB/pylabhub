/**
 * @file test_hub_state.cpp
 * @brief L2 unit tests for HubState skeleton (HEP-0033 §G2.1).
 *
 * Phase G2.1 ships HubState with its full read/write/event surface, but
 * nothing calls mutators from production code yet — that's G2.2. These
 * tests exercise the surface directly via a `friend` test-shim
 * (`HubStateTestAccess`, forward-declared in hub_state.hpp) so that
 * private `_set_*` mutators can be driven from test code without
 * loosening access on the public API.
 *
 * What's covered here:
 *   - empty snapshot / missing-lookup shape
 *   - set_channel_opened → snapshot + channel(name) agree; handler fires
 *   - status transition + close → handlers fire exactly once per op
 *   - consumer add / remove → list + handler match
 *   - role register / heartbeat / disconnect → state + handler agree
 *   - band join / leave → members list + handlers agree; empty band evicted
 *   - peer connect / disconnect → state + handler agree
 *   - unsubscribe stops further handler invocations
 *   - concurrent reader while single writer mutates — basic sanity
 *     (no data race detectable under TSan; handlers still fire)
 *
 * G2.2 will add end-to-end tests driven through BrokerService inbound
 * handlers; this file intentionally stops at the mutator contract.
 */

#include "utils/hub_state.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace pylabhub::hub::test
{

/// Friend shim. Forwards to private `_set_*` mutators so the tests do
/// not need broker plumbing. HubState declares this `friend`.
struct HubStateTestAccess
{
    static void set_channel_opened(HubState &s, ChannelEntry e)
    {
        s._set_channel_opened(std::move(e));
    }
    static void set_channel_status(HubState &s, const std::string &name, ChannelStatus st)
    {
        s._set_channel_status(name, st);
    }
    static void set_channel_closed(HubState &s, const std::string &name)
    {
        s._set_channel_closed(name);
    }
    static void add_consumer(HubState &s, const std::string &ch, ConsumerEntry c)
    {
        s._add_consumer(ch, std::move(c));
    }
    static void remove_consumer(HubState &s, const std::string &ch, const std::string &uid)
    {
        s._remove_consumer(ch, uid);
    }
    static void set_role_registered(HubState &s, RoleEntry e)
    {
        s._set_role_registered(std::move(e));
    }
    static void update_role_heartbeat(HubState                             &s,
                                      const std::string                    &uid,
                                      std::chrono::steady_clock::time_point when)
    {
        s._update_role_heartbeat(uid, when);
    }
    static void set_role_disconnected(HubState &s, const std::string &uid)
    {
        s._set_role_disconnected(uid);
    }
    static void set_band_joined(HubState &s, const std::string &b, BandMember m)
    {
        s._set_band_joined(b, std::move(m));
    }
    static void set_band_left(HubState &s, const std::string &b, const std::string &uid)
    {
        s._set_band_left(b, uid);
    }
    static void set_peer_connected(HubState &s, PeerEntry e)
    {
        s._set_peer_connected(std::move(e));
    }
    static void set_peer_disconnected(HubState &s, const std::string &uid)
    {
        s._set_peer_disconnected(uid);
    }
    static void set_shm_block(HubState &s, ShmBlockRef r)
    {
        s._set_shm_block(std::move(r));
    }
    static void bump_counter(HubState &s, const std::string &k, uint64_t n = 1)
    {
        s._bump_counter(k, n);
    }
};

} // namespace pylabhub::hub::test

using pylabhub::hub::BandEntry;
using pylabhub::hub::BandMember;
using pylabhub::hub::ChannelEntry;
using pylabhub::hub::ChannelStatus;
using pylabhub::hub::ConsumerEntry;
using pylabhub::hub::HandlerId;
using pylabhub::hub::HubState;
using pylabhub::hub::HubStateSnapshot;
using pylabhub::hub::kInvalidHandlerId;
using pylabhub::hub::PeerEntry;
using pylabhub::hub::PeerState;
using pylabhub::hub::RoleEntry;
using pylabhub::hub::RoleState;
using pylabhub::hub::ShmBlockRef;
using pylabhub::hub::test::HubStateTestAccess;

namespace
{

ChannelEntry make_channel(const std::string &name)
{
    ChannelEntry e;
    e.name               = name;
    e.shm_name           = name + "-shm";
    e.schema_hash        = std::string(64, 'a');
    e.schema_version     = 1;
    e.producer_pid       = 4242;
    e.producer_role_uid  = "prod-uid";
    e.producer_role_name = "prod-name";
    e.status             = ChannelStatus::PendingReady;
    return e;
}

ConsumerEntry make_consumer(const std::string &uid, uint64_t pid = 1234)
{
    ConsumerEntry c;
    c.consumer_pid = pid;
    c.role_uid     = uid;
    c.role_name    = uid + "-name";
    return c;
}

RoleEntry make_role(const std::string &uid, const std::string &tag = "prod")
{
    RoleEntry r;
    r.uid      = uid;
    r.name     = uid + "-name";
    r.role_tag = tag;
    return r;
}

PeerEntry make_peer(const std::string &uid)
{
    PeerEntry p;
    p.uid      = uid;
    p.endpoint = "tcp://hub-" + uid + ":5570";
    return p;
}

} // namespace

// ─── Empty state ────────────────────────────────────────────────────────────

TEST(HubStateSkeleton, EmptySnapshotIsEmpty)
{
    HubState s;
    auto     snap = s.snapshot();
    EXPECT_TRUE(snap.channels.empty());
    EXPECT_TRUE(snap.roles.empty());
    EXPECT_TRUE(snap.bands.empty());
    EXPECT_TRUE(snap.peers.empty());
    EXPECT_TRUE(snap.shm_blocks.empty());
    EXPECT_EQ(snap.counters.ready_to_pending_total, 0u);
}

TEST(HubStateSkeleton, MissingLookupsReturnNullopt)
{
    HubState s;
    EXPECT_FALSE(s.channel("nope").has_value());
    EXPECT_FALSE(s.role("nope").has_value());
    EXPECT_FALSE(s.band("nope").has_value());
    EXPECT_FALSE(s.peer("nope").has_value());
    EXPECT_FALSE(s.shm_block("nope").has_value());
}

// ─── Channels ───────────────────────────────────────────────────────────────

TEST(HubStateSkeleton, ChannelOpened_PopulatesAndFires)
{
    HubState s;

    std::vector<std::string> fired;
    HandlerId                id = s.subscribe_channel_opened(
        [&](const ChannelEntry &e) { fired.push_back(e.name); });
    ASSERT_NE(id, kInvalidHandlerId);

    HubStateTestAccess::set_channel_opened(s, make_channel("ch1"));

    auto got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "ch1");
    EXPECT_EQ(got->status, ChannelStatus::PendingReady);

    ASSERT_EQ(fired.size(), 1u);
    EXPECT_EQ(fired[0], "ch1");

    auto snap = s.snapshot();
    EXPECT_EQ(snap.channels.count("ch1"), 1u);
}

TEST(HubStateSkeleton, ChannelStatusChange_FiresOnce)
{
    HubState s;
    HubStateTestAccess::set_channel_opened(s, make_channel("ch1"));

    int fired = 0;
    s.subscribe_channel_status_changed([&](const ChannelEntry &e) {
        EXPECT_EQ(e.name, "ch1");
        EXPECT_EQ(e.status, ChannelStatus::Ready);
        ++fired;
    });

    HubStateTestAccess::set_channel_status(s, "ch1", ChannelStatus::Ready);
    EXPECT_EQ(fired, 1);

    auto got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, ChannelStatus::Ready);
}

TEST(HubStateSkeleton, ChannelStatusChange_MissingChannelIsNoop)
{
    HubState s;
    int      fired = 0;
    s.subscribe_channel_status_changed([&](const ChannelEntry &) { ++fired; });
    HubStateTestAccess::set_channel_status(s, "nope", ChannelStatus::Ready);
    EXPECT_EQ(fired, 0);
}

TEST(HubStateSkeleton, ChannelClosed_RemovesAndFires)
{
    HubState s;
    HubStateTestAccess::set_channel_opened(s, make_channel("ch1"));

    std::string last;
    s.subscribe_channel_closed([&](const std::string &name) { last = name; });

    HubStateTestAccess::set_channel_closed(s, "ch1");
    EXPECT_EQ(last, "ch1");
    EXPECT_FALSE(s.channel("ch1").has_value());

    // Closing a missing channel is silently dropped — handler does not refire.
    last.clear();
    HubStateTestAccess::set_channel_closed(s, "ch1");
    EXPECT_EQ(last, "");
}

// ─── Consumers ──────────────────────────────────────────────────────────────

TEST(HubStateSkeleton, ConsumerAddRemove_UpdatesChannelAndFires)
{
    HubState s;
    HubStateTestAccess::set_channel_opened(s, make_channel("ch1"));

    std::vector<std::string> added;
    std::vector<std::string> removed;
    s.subscribe_consumer_added(
        [&](const std::string &, const ConsumerEntry &c) { added.push_back(c.role_uid); });
    s.subscribe_consumer_removed(
        [&](const std::string &, const std::string &uid) { removed.push_back(uid); });

    HubStateTestAccess::add_consumer(s, "ch1", make_consumer("c-A", 1));
    HubStateTestAccess::add_consumer(s, "ch1", make_consumer("c-B", 2));
    auto got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->consumers.size(), 2u);

    HubStateTestAccess::remove_consumer(s, "ch1", "c-A");
    got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->consumers.size(), 1u);
    EXPECT_EQ(got->consumers[0].role_uid, "c-B");

    EXPECT_EQ(added, (std::vector<std::string>{"c-A", "c-B"}));
    EXPECT_EQ(removed, (std::vector<std::string>{"c-A"}));
}

TEST(HubStateSkeleton, ConsumerAdd_DedupesSameUid)
{
    HubState s;
    HubStateTestAccess::set_channel_opened(s, make_channel("ch1"));
    HubStateTestAccess::add_consumer(s, "ch1", make_consumer("c-A", 100));
    HubStateTestAccess::add_consumer(s, "ch1", make_consumer("c-A", 200));

    auto got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->consumers.size(), 1u);
    EXPECT_EQ(got->consumers[0].consumer_pid, 200u);
}

// ─── Roles ──────────────────────────────────────────────────────────────────

TEST(HubStateSkeleton, RoleRegistered_FiresAndStored)
{
    HubState s;

    std::string last_uid;
    s.subscribe_role_registered([&](const RoleEntry &e) { last_uid = e.uid; });

    HubStateTestAccess::set_role_registered(s, make_role("r1"));
    EXPECT_EQ(last_uid, "r1");
    auto r = s.role("r1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->state, RoleState::Connected);
}

TEST(HubStateSkeleton, RoleHeartbeat_RevivesDisconnected)
{
    HubState s;
    HubStateTestAccess::set_role_registered(s, make_role("r1"));
    HubStateTestAccess::set_role_disconnected(s, "r1");
    ASSERT_EQ(s.role("r1")->state, RoleState::Disconnected);

    HubStateTestAccess::update_role_heartbeat(s, "r1",
                                              std::chrono::steady_clock::now());
    EXPECT_EQ(s.role("r1")->state, RoleState::Connected);
}

TEST(HubStateSkeleton, RoleDisconnected_FiresOnlyOnceUntilRevived)
{
    HubState s;
    HubStateTestAccess::set_role_registered(s, make_role("r1"));

    int fired = 0;
    s.subscribe_role_disconnected([&](const std::string &uid) {
        EXPECT_EQ(uid, "r1");
        ++fired;
    });

    HubStateTestAccess::set_role_disconnected(s, "r1");
    HubStateTestAccess::set_role_disconnected(s, "r1"); // idempotent
    EXPECT_EQ(fired, 1);

    HubStateTestAccess::update_role_heartbeat(s, "r1",
                                              std::chrono::steady_clock::now());
    HubStateTestAccess::set_role_disconnected(s, "r1");
    EXPECT_EQ(fired, 2);
}

// ─── Bands ──────────────────────────────────────────────────────────────────

TEST(HubStateSkeleton, BandJoinLeave_MembershipAndHandlers)
{
    HubState s;

    std::vector<std::string> joined;
    std::vector<std::string> left;
    s.subscribe_band_joined(
        [&](const std::string &, const BandMember &m) { joined.push_back(m.role_uid); });
    s.subscribe_band_left(
        [&](const std::string &, const std::string &uid) { left.push_back(uid); });

    BandMember m;
    m.role_uid     = "r1";
    m.role_name    = "r1-n";
    m.zmq_identity = "zmq1";
    HubStateTestAccess::set_band_joined(s, "band", m);

    BandMember m2;
    m2.role_uid     = "r2";
    m2.role_name    = "r2-n";
    m2.zmq_identity = "zmq2";
    HubStateTestAccess::set_band_joined(s, "band", m2);

    auto b = s.band("band");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->members.size(), 2u);

    HubStateTestAccess::set_band_left(s, "band", "r1");
    b = s.band("band");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->members.size(), 1u);
    EXPECT_EQ(b->members[0].role_uid, "r2");

    HubStateTestAccess::set_band_left(s, "band", "r2");
    // Band empties → entry evicted per G2 spec.
    EXPECT_FALSE(s.band("band").has_value());

    EXPECT_EQ(joined, (std::vector<std::string>{"r1", "r2"}));
    EXPECT_EQ(left, (std::vector<std::string>{"r1", "r2"}));
}

// ─── Peers ──────────────────────────────────────────────────────────────────

TEST(HubStateSkeleton, PeerConnectDisconnect_FiresAndTracks)
{
    HubState s;

    std::string last_conn;
    std::string last_disc;
    s.subscribe_peer_connected(
        [&](const PeerEntry &p) { last_conn = p.uid; });
    s.subscribe_peer_disconnected(
        [&](const std::string &uid) { last_disc = uid; });

    HubStateTestAccess::set_peer_connected(s, make_peer("p1"));
    auto got = s.peer("p1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->state, PeerState::Connected);
    EXPECT_EQ(last_conn, "p1");

    HubStateTestAccess::set_peer_disconnected(s, "p1");
    got = s.peer("p1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->state, PeerState::Disconnected);
    EXPECT_EQ(last_disc, "p1");
}

// ─── SHM block + counters ───────────────────────────────────────────────────

TEST(HubStateSkeleton, ShmBlock_InsertOrAssign)
{
    HubState    s;
    ShmBlockRef r{"ch1", "/dev/shm/plh-ch1"};
    HubStateTestAccess::set_shm_block(s, r);
    auto got = s.shm_block("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->block_path, "/dev/shm/plh-ch1");
}

TEST(HubStateSkeleton, BumpCounter_Accumulates)
{
    HubState s;
    HubStateTestAccess::bump_counter(s, "REG_REQ");
    HubStateTestAccess::bump_counter(s, "REG_REQ", 3);
    HubStateTestAccess::bump_counter(s, "HEARTBEAT_REQ");
    auto c = s.counters();
    EXPECT_EQ(c.msg_type_counts.at("REG_REQ"), 4u);
    EXPECT_EQ(c.msg_type_counts.at("HEARTBEAT_REQ"), 1u);
}

// ─── Unsubscribe ────────────────────────────────────────────────────────────

TEST(HubStateSkeleton, Unsubscribe_StopsFurtherCalls)
{
    HubState s;

    int       fired = 0;
    HandlerId id    = s.subscribe_channel_opened(
        [&](const ChannelEntry &) { ++fired; });
    HubStateTestAccess::set_channel_opened(s, make_channel("ch1"));
    EXPECT_EQ(fired, 1);

    s.unsubscribe(id);
    HubStateTestAccess::set_channel_opened(s, make_channel("ch2"));
    EXPECT_EQ(fired, 1);

    // Unsubscribing an invalid / already-removed id is a silent noop.
    s.unsubscribe(id);
    s.unsubscribe(kInvalidHandlerId);
    s.unsubscribe(999999);
}

// ─── Concurrency sanity ─────────────────────────────────────────────────────

TEST(HubStateSkeleton, ConcurrentReadersDoNotRaceWithSingleWriter)
{
    HubState s;
    HubStateTestAccess::set_channel_opened(s, make_channel("ch1"));

    std::atomic<bool> stop{false};
    std::atomic<int>  reads{0};

    std::thread writer([&] {
        for (int i = 0; i < 2000 && !stop.load(); ++i)
        {
            HubStateTestAccess::set_channel_status(
                s, "ch1",
                (i & 1) ? ChannelStatus::Ready : ChannelStatus::PendingReady);
            HubStateTestAccess::bump_counter(s, "tick");
        }
        stop.store(true);
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t)
    {
        readers.emplace_back([&] {
            while (!stop.load())
            {
                auto ch = s.channel("ch1");
                if (ch.has_value())
                {
                    // Value-copy; must be a valid enum regardless of writer race.
                    auto st = ch->status;
                    EXPECT_TRUE(st == ChannelStatus::Ready ||
                                st == ChannelStatus::PendingReady);
                    reads.fetch_add(1);
                }
            }
        });
    }

    writer.join();
    for (auto &r : readers) r.join();
    EXPECT_GT(reads.load(), 0);
}
