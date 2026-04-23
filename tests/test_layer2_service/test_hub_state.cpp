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

#include "utils/broker_service.hpp"
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

    // ── Capability-operation forwarders (HEP-0033 §G2, G2.2.0) ──────────
    static void on_channel_registered(HubState &s, ChannelEntry e)
    {
        s._on_channel_registered(std::move(e));
    }
    static void on_channel_closed(HubState &s, const std::string &n,
                                  ChannelCloseReason why)
    {
        s._on_channel_closed(n, why);
    }
    static void on_consumer_joined(HubState &s, const std::string &ch,
                                   ConsumerEntry c)
    {
        s._on_consumer_joined(ch, std::move(c));
    }
    static void on_consumer_left(HubState &s, const std::string &ch,
                                 const std::string &uid)
    {
        s._on_consumer_left(ch, uid);
    }
    static void on_heartbeat(HubState                                 &s,
                             const std::string                        &ch,
                             const std::string                        &uid,
                             std::chrono::steady_clock::time_point     when,
                             const std::optional<nlohmann::json>      &m)
    {
        s._on_heartbeat(ch, uid, when, m);
    }
    static void on_heartbeat_timeout(HubState &s, const std::string &ch,
                                     const std::string &uid)
    {
        s._on_heartbeat_timeout(ch, uid);
    }
    static void on_pending_timeout(HubState &s, const std::string &ch)
    {
        s._on_pending_timeout(ch);
    }
    static void on_metrics_reported(HubState                             &s,
                                    const std::string                    &ch,
                                    const std::string                    &uid,
                                    nlohmann::json                        m,
                                    std::chrono::system_clock::time_point when)
    {
        s._on_metrics_reported(ch, uid, std::move(m), when);
    }
    static void on_band_joined(HubState &s, const std::string &band,
                               BandMember m)
    {
        s._on_band_joined(band, std::move(m));
    }
    static void on_band_left(HubState &s, const std::string &band,
                             const std::string &uid)
    {
        s._on_band_left(band, uid);
    }
    static void on_peer_connected(HubState &s, PeerEntry p)
    {
        s._on_peer_connected(std::move(p));
    }
    static void on_peer_disconnected(HubState &s, const std::string &uid)
    {
        s._on_peer_disconnected(uid);
    }
    static void on_message_processed(HubState          &s,
                                     const std::string &msg_type,
                                     std::size_t        in,
                                     std::size_t        out)
    {
        s._on_message_processed(msg_type, in, out);
    }
};

} // namespace pylabhub::hub::test

using pylabhub::hub::BandEntry;
using pylabhub::hub::BandMember;
using pylabhub::hub::ChannelCloseReason;
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

// All identifiers in these tests follow HEP-0033 §G2.2.0b naming rules
// (role.uid ≥3 components; `@` for peers; `!` for bands; channel first
// component not in {prod,cons,proc,sys}). Helpers below take the bare
// identifier — the caller is responsible for passing a valid form.

ChannelEntry make_channel(const std::string &name)
{
    ChannelEntry e;
    e.name               = name;
    e.shm_name           = name + "-shm";
    e.schema_hash        = std::string(64, 'a');
    e.schema_version     = 1;
    e.producer_pid       = 4242;
    e.producer_role_uid  = "prod.main.test";
    e.producer_role_name = "main";
    e.status             = ChannelStatus::PendingReady;
    return e;
}

ConsumerEntry make_consumer(const std::string &uid, uint64_t pid = 1234)
{
    ConsumerEntry c;
    c.consumer_pid = pid;
    c.role_uid     = uid;  // must be a valid role.uid per caller
    c.role_name    = uid;  // display; tests that care override
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
    p.uid      = uid;  // must be a valid peer id per caller (leading '@')
    p.endpoint = "tcp://hub.test:5570";
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

    HubStateTestAccess::add_consumer(s, "ch1", make_consumer("cons.A.test", 1));
    HubStateTestAccess::add_consumer(s, "ch1", make_consumer("cons.B.test", 2));
    auto got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->consumers.size(), 2u);

    HubStateTestAccess::remove_consumer(s, "ch1", "cons.A.test");
    got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->consumers.size(), 1u);
    EXPECT_EQ(got->consumers[0].role_uid, "cons.B.test");

    EXPECT_EQ(added, (std::vector<std::string>{"cons.A.test", "cons.B.test"}));
    EXPECT_EQ(removed, (std::vector<std::string>{"cons.A.test"}));
}

TEST(HubStateSkeleton, ConsumerAdd_DedupesSameUid)
{
    HubState s;
    HubStateTestAccess::set_channel_opened(s, make_channel("ch1"));
    HubStateTestAccess::add_consumer(s, "ch1", make_consumer("cons.A.test", 100));
    HubStateTestAccess::add_consumer(s, "ch1", make_consumer("cons.A.test", 200));

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

    HubStateTestAccess::set_role_registered(s, make_role("prod.r1.test"));
    EXPECT_EQ(last_uid, "prod.r1.test");
    auto r = s.role("prod.r1.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->state, RoleState::Connected);
}

TEST(HubStateSkeleton, RoleHeartbeat_RevivesDisconnected)
{
    HubState s;
    HubStateTestAccess::set_role_registered(s, make_role("prod.r1.test"));
    HubStateTestAccess::set_role_disconnected(s, "prod.r1.test");
    ASSERT_EQ(s.role("prod.r1.test")->state, RoleState::Disconnected);

    HubStateTestAccess::update_role_heartbeat(s, "prod.r1.test",
                                              std::chrono::steady_clock::now());
    EXPECT_EQ(s.role("prod.r1.test")->state, RoleState::Connected);
}

TEST(HubStateSkeleton, RoleDisconnected_FiresOnlyOnceUntilRevived)
{
    HubState s;
    HubStateTestAccess::set_role_registered(s, make_role("prod.r1.test"));

    int fired = 0;
    s.subscribe_role_disconnected([&](const std::string &uid) {
        EXPECT_EQ(uid, "prod.r1.test");
        ++fired;
    });

    HubStateTestAccess::set_role_disconnected(s, "prod.r1.test");
    HubStateTestAccess::set_role_disconnected(s, "prod.r1.test"); // idempotent
    EXPECT_EQ(fired, 1);

    HubStateTestAccess::update_role_heartbeat(s, "prod.r1.test",
                                              std::chrono::steady_clock::now());
    HubStateTestAccess::set_role_disconnected(s, "prod.r1.test");
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
    m.role_uid     = "prod.r1.test";
    m.role_name    = "r1";
    m.zmq_identity = "zmq1";
    HubStateTestAccess::set_band_joined(s, "!band", m);

    BandMember m2;
    m2.role_uid     = "prod.r2.test";
    m2.role_name    = "r2";
    m2.zmq_identity = "zmq2";
    HubStateTestAccess::set_band_joined(s, "!band", m2);

    auto b = s.band("!band");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->members.size(), 2u);

    HubStateTestAccess::set_band_left(s, "!band", "prod.r1.test");
    b = s.band("!band");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->members.size(), 1u);
    EXPECT_EQ(b->members[0].role_uid, "prod.r2.test");

    HubStateTestAccess::set_band_left(s, "!band", "prod.r2.test");
    // Band empties → entry evicted per G2 spec.
    EXPECT_FALSE(s.band("!band").has_value());

    EXPECT_EQ(joined, (std::vector<std::string>{"prod.r1.test", "prod.r2.test"}));
    EXPECT_EQ(left,   (std::vector<std::string>{"prod.r1.test", "prod.r2.test"}));
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

    HubStateTestAccess::set_peer_connected(s, make_peer("hub.p1.test"));
    auto got = s.peer("hub.p1.test");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->state, PeerState::Connected);
    EXPECT_EQ(last_conn, "hub.p1.test");

    HubStateTestAccess::set_peer_disconnected(s, "hub.p1.test");
    got = s.peer("hub.p1.test");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->state, PeerState::Disconnected);
    EXPECT_EQ(last_disc, "hub.p1.test");
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
    std::atomic<int>  readers_ready{0};
    constexpr int     kReaderCount = 4;

    std::vector<std::thread> readers;
    for (int t = 0; t < kReaderCount; ++t)
    {
        readers.emplace_back([&] {
            // Handshake: signal readiness so the writer doesn't race ahead.
            readers_ready.fetch_add(1);
            while (!stop.load())
            {
                auto ch = s.channel("ch1");
                if (ch.has_value())
                {
                    // Value-copy; must be a valid enum regardless of writer race.
                    auto st = ch->status;
                    EXPECT_TRUE(st == ChannelStatus::Ready ||
                                st == ChannelStatus::PendingReady);
                }
            }
        });
    }

    // Wait for all readers to be in the poll loop before the writer races.
    while (readers_ready.load() < kReaderCount) std::this_thread::yield();

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

    writer.join();
    for (auto &r : readers) r.join();

    // No EXPECT on read count — ASan/TSan builds catch data races directly;
    // "how many reads happened" is a timing property, not a correctness one.
}

// ═══ Capability-operation layer (HEP-0033 §G2, G2.2.0) ══════════════════════
//
// These tests verify that each `_on_*` op composes its primitives correctly.
// The primitives themselves are exercised above; here we only check that the
// op wires them together in the right order with the right field derivations.

TEST(HubStateOps, ChannelRegistered_ComposesChannelAndRoleAndShmAndCounter)
{
    HubState s;

    std::vector<std::string> opened_fired;
    std::vector<std::string> role_fired;
    s.subscribe_channel_opened(
        [&](const ChannelEntry &e) { opened_fired.push_back(e.name); });
    s.subscribe_role_registered(
        [&](const RoleEntry &r) { role_fired.push_back(r.uid); });

    auto ch            = make_channel("ch1");
    ch.has_shared_memory = true;
    ch.zmq_pubkey      = "pubkey-xyz";
    HubStateTestAccess::on_channel_registered(s, ch);

    // Channel present.
    auto got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "ch1");

    // Producer RoleEntry derived.  Name + role_tag are derived from
    // uid's components, not copied from `ChannelEntry.producer_role_name`
    // (which is only a hint — the uid is the source of truth).
    auto r = s.role("prod.main.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->role_tag,   "prod");
    EXPECT_EQ(r->name,       "main");    // second uid component
    EXPECT_EQ(r->pubkey_z85, "pubkey-xyz");
    EXPECT_EQ(r->state, RoleState::Connected);
    ASSERT_EQ(r->channels.size(), 1u);
    EXPECT_EQ(r->channels[0], "ch1");

    // SHM block registered.
    auto shm = s.shm_block("ch1");
    ASSERT_TRUE(shm.has_value());
    EXPECT_EQ(shm->block_path, "ch1-shm");

    // Counter bumped.
    EXPECT_EQ(s.counters().msg_type_counts.at("REG_REQ"), 1u);

    // Events for channel + role both fired.
    EXPECT_EQ(opened_fired, (std::vector<std::string>{"ch1"}));
    EXPECT_EQ(role_fired, (std::vector<std::string>{"prod.main.test"}));
}

TEST(HubStateOps, ChannelRegistered_NoShm_SkipsShmBlock)
{
    HubState s;
    auto     ch            = make_channel("ch1");
    ch.has_shared_memory = false;
    HubStateTestAccess::on_channel_registered(s, ch);
    EXPECT_FALSE(s.shm_block("ch1").has_value());
}

TEST(HubStateOps, ChannelRegistered_SameProducerOnTwoChannels_MergesRoleChannels)
{
    HubState s;
    auto     c1            = make_channel("ch1");
    auto     c2            = make_channel("ch2");
    // Both share producer_role_uid = "prod.main.test" from make_channel helper.
    HubStateTestAccess::on_channel_registered(s, c1);
    HubStateTestAccess::on_channel_registered(s, c2);

    auto r = s.role("prod.main.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->channels.size(), 2u);
    EXPECT_EQ(r->state, RoleState::Connected);
}

TEST(HubStateOps, ChannelRegistered_EmptyProducerUid_SkipsRole)
{
    HubState s;
    auto     ch                = make_channel("ch1");
    ch.producer_role_uid     = "";
    HubStateTestAccess::on_channel_registered(s, ch);
    // Channel present; no RoleEntry inserted.
    EXPECT_TRUE(s.channel("ch1").has_value());
    EXPECT_TRUE(s.snapshot().roles.empty());
}

TEST(HubStateOps, ChannelClosed_RemovesChannelAndRolesChannelRef)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    ASSERT_EQ(s.role("prod.main.test")->channels.size(), 1u);

    HubStateTestAccess::on_channel_closed(
        s, "ch1", ChannelCloseReason::VoluntaryDereg);
    EXPECT_FALSE(s.channel("ch1").has_value());
    EXPECT_FALSE(s.shm_block("ch1").has_value());

    auto r = s.role("prod.main.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->channels.empty());

    // Counter key encodes reason.
    EXPECT_EQ(s.counters().msg_type_counts.at("close:VoluntaryDereg"), 1u);
}

TEST(HubStateOps, ChannelClosed_MultiChannel_OnlyScrubsClosedChannelFromRole)
{
    HubState s;
    auto     c1 = make_channel("ch1");
    auto     c2 = make_channel("ch2");
    // Both carry producer_role_uid = "prod.main.test" (via make_channel helper),
    // so the same RoleEntry ends up with channels = ["ch1","ch2"].
    HubStateTestAccess::on_channel_registered(s, c1);
    HubStateTestAccess::on_channel_registered(s, c2);
    ASSERT_EQ(s.role("prod.main.test")->channels.size(), 2u);

    // Close only ch1; ch2 must remain in the role's channels list.
    HubStateTestAccess::on_channel_closed(
        s, "ch1", ChannelCloseReason::VoluntaryDereg);
    EXPECT_FALSE(s.channel("ch1").has_value());
    EXPECT_TRUE(s.channel("ch2").has_value());

    auto r = s.role("prod.main.test");
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->channels.size(), 1u);
    EXPECT_EQ(r->channels[0], "ch2");
}

TEST(HubStateOps, ConsumerJoined_UpsertsConsumerAndConsumerRole)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_consumer_joined(s, "ch1", make_consumer("cons.A.test"));

    auto ch = s.channel("ch1");
    ASSERT_TRUE(ch.has_value());
    ASSERT_EQ(ch->consumers.size(), 1u);
    EXPECT_EQ(ch->consumers[0].role_uid, "cons.A.test");

    auto r = s.role("cons.A.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->role_tag, "cons");
    EXPECT_EQ(r->name,     "A");        // derived from uid's second component
    EXPECT_EQ(r->channels.size(), 1u);
    EXPECT_EQ(r->channels[0], "ch1");
}

TEST(HubStateOps, ConsumerLeft_RemovesFromChannelAndRoleChannelsList)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_consumer_joined(s, "ch1", make_consumer("cons.A.test"));
    HubStateTestAccess::on_consumer_left(s, "ch1", "cons.A.test");

    auto ch = s.channel("ch1");
    ASSERT_TRUE(ch.has_value());
    EXPECT_TRUE(ch->consumers.empty());

    // Role remains (consumer role may still be active elsewhere) but its
    // channel reference for ch1 is gone.
    auto r = s.role("cons.A.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->channels.empty());
    EXPECT_EQ(r->state, RoleState::Connected);
}

TEST(HubStateOps, Heartbeat_TransitionsPendingToReady_FiresOnce)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    int ready_fires = 0;
    s.subscribe_channel_status_changed([&](const ChannelEntry &e) {
        EXPECT_EQ(e.status, ChannelStatus::Ready);
        ++ready_fires;
    });

    const auto t1 = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", t1, std::nullopt);
    EXPECT_EQ(ready_fires, 1);
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::Ready);

    // Second heartbeat is a plain last_heartbeat refresh — no further fire.
    const auto t2 = t1 + std::chrono::milliseconds(10);
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", t2, std::nullopt);
    EXPECT_EQ(ready_fires, 1);
}

TEST(HubStateOps, Heartbeat_WithMetrics_StoresOnRole)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    nlohmann::json metrics = {{"qps", 123}, {"errors", 0}};
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test",
                                     std::chrono::steady_clock::now(), metrics);

    auto r = s.role("prod.main.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->latest_metrics, metrics);
    EXPECT_NE(r->metrics_collected_at, std::chrono::system_clock::time_point{});
}

TEST(HubStateOps, HeartbeatTimeout_DemotesAndBumpsCounter)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", std::chrono::steady_clock::now(), std::nullopt);
    ASSERT_EQ(s.channel("ch1")->status, ChannelStatus::Ready);

    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::PendingReady);
    EXPECT_EQ(s.role("prod.main.test")->state, RoleState::Disconnected);
    EXPECT_EQ(s.counters().ready_to_pending_total, 1u);
}

TEST(HubStateOps, PendingTimeout_ClosingAndBumpsCounter)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_pending_timeout(s, "ch1");
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::Closing);
    EXPECT_EQ(s.counters().pending_to_deregistered_total, 1u);
}

TEST(HubStateOps, BandJoined_UpsertsMemberRole)
{
    HubState s;
    BandMember m;
    m.role_uid     = "prod.r1.test";
    m.role_name    = "r1-name";
    m.zmq_identity = "zmq-id";
    HubStateTestAccess::on_band_joined(s, "!band", m);

    auto b = s.band("!band");
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(b->members.size(), 1u);
    EXPECT_EQ(b->members[0].role_uid, "prod.r1.test");

    auto r = s.role("prod.r1.test");
    ASSERT_TRUE(r.has_value());
    // name is derived from uid's second component ('r1'), not from the
    // separate role_name field on BandMember.
    EXPECT_EQ(r->name, "r1");
    EXPECT_EQ(r->role_tag, "prod");
    EXPECT_TRUE(r->channels.empty()); // band membership doesn't populate channels
    EXPECT_EQ(s.counters().msg_type_counts.at("BAND_JOIN_REQ"), 1u);
}

TEST(HubStateOps, PeerConnected_InsertsAndBumpsCounter)
{
    HubState s;
    HubStateTestAccess::on_peer_connected(s, make_peer("hub.p1.test"));
    auto p = s.peer("hub.p1.test");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->state, PeerState::Connected);
    EXPECT_EQ(s.counters().msg_type_counts.at("HUB_PEER_HELLO"), 1u);
}

TEST(HubStateOps, MetricsReported_StoresOnRoleWithoutLivenessSideEffect)
{
    HubState s;
    // Role enters via channel registration + one heartbeat so it's Ready.
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    const auto hb_time = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", hb_time, std::nullopt);
    const auto role_hb_before = s.role("prod.main.test")->last_heartbeat;

    // METRICS_REPORT_REQ arrives later.
    nlohmann::json m   = {{"rx_count", 42}};
    const auto     now = std::chrono::system_clock::now();
    HubStateTestAccess::on_metrics_reported(s, "ch1", "prod.main.test", m, now);

    auto r = s.role("prod.main.test");
    ASSERT_TRUE(r.has_value());
    // Metrics got stored.
    EXPECT_EQ(r->latest_metrics, m);
    EXPECT_EQ(r->metrics_collected_at, now);
    // Crucially: metrics report does NOT bump the role's liveness clock.
    EXPECT_EQ(r->last_heartbeat, role_hb_before);

    EXPECT_EQ(s.counters().msg_type_counts.at("METRICS_REPORT_REQ"), 1u);
}

TEST(HubStateOps, MessageProcessed_BumpsCounterAndBytes)
{
    HubState s;
    HubStateTestAccess::on_message_processed(s, "REG_REQ", 128, 64);
    HubStateTestAccess::on_message_processed(s, "REG_REQ", 128, 64);
    HubStateTestAccess::on_message_processed(s, "DISC_REQ", 80, 16);

    auto c = s.counters();
    EXPECT_EQ(c.msg_type_counts.at("REG_REQ"), 2u);
    EXPECT_EQ(c.msg_type_counts.at("DISC_REQ"), 1u);
    EXPECT_EQ(c.bytes_in_total,  128u + 128u + 80u);
    EXPECT_EQ(c.bytes_out_total, 64u  + 64u  + 16u);
}

// ═══ HubState validator wiring (HEP-0033 §G2.2.0b) ═════════════════════════
//
// Every `_on_*` op validates its identifiers at entry and drops the
// mutation if any is malformed, bumping the `sys.invalid_identifier_rejected`
// counter. These two tests pin that wiring: a future change that
// accidentally removes the guard would make them fail, even though
// the naming validator tests (test_naming.cpp) would still pass.

TEST(HubStateValidation, ChannelRegistered_InvalidChannelName_DroppedAndCounted)
{
    HubState s;
    ChannelEntry bad = make_channel("prod.oops"); // reserved first component → invalid channel
    HubStateTestAccess::on_channel_registered(s, bad);

    EXPECT_FALSE(s.channel("prod.oops").has_value());
    EXPECT_TRUE(s.snapshot().roles.empty());
    EXPECT_TRUE(s.snapshot().shm_blocks.empty());

    const auto &counts = s.counters().msg_type_counts;
    auto it = counts.find("sys.invalid_identifier_rejected");
    ASSERT_NE(it, counts.end());
    EXPECT_EQ(it->second, 1u);
    // REG_REQ counter must NOT be bumped — the op bailed before the tail.
    EXPECT_EQ(counts.count("REG_REQ"), 0u);
}

TEST(HubStateValidation, ConsumerJoined_InvalidRoleUid_DroppedAndCounted)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    ConsumerEntry c;
    c.consumer_pid = 1;
    c.role_uid     = "bogus-uid"; // missing tag prefix → invalid RoleUid
    HubStateTestAccess::on_consumer_joined(s, "ch1", c);

    auto ch = s.channel("ch1");
    ASSERT_TRUE(ch.has_value());
    EXPECT_TRUE(ch->consumers.empty());

    // No RoleEntry for the bogus uid.
    EXPECT_FALSE(s.role("bogus-uid").has_value());

    const auto &counts = s.counters().msg_type_counts;
    EXPECT_EQ(counts.at("sys.invalid_identifier_rejected"), 1u);
    EXPECT_EQ(counts.count("CONSUMER_REG_REQ"), 0u);
}

// ═══ BrokerService plumbing (HEP-0033 §G2.2.0) ══════════════════════════════

/// G2.2.0 only plumbs `HubState` into `BrokerServiceImpl` and exposes it;
/// no broker handler is yet mutating it. The accessor test verifies the
/// reference is stable and the state starts empty. End-to-end "broker
/// mutates HubState via `_on_*`" coverage lands in G2.2.1+.
TEST(BrokerServicePlumbing, HubStateAccessorReturnsEmptyAggregate)
{
    pylabhub::broker::BrokerService::Config cfg;
    cfg.endpoint  = "tcp://127.0.0.1:0";
    cfg.use_curve = false;
    pylabhub::broker::BrokerService broker(cfg);

    const auto &state = broker.hub_state();
    auto        snap  = state.snapshot();
    EXPECT_TRUE(snap.channels.empty());
    EXPECT_TRUE(snap.roles.empty());
    EXPECT_TRUE(snap.bands.empty());
    EXPECT_TRUE(snap.peers.empty());
    EXPECT_TRUE(snap.shm_blocks.empty());

    // The accessor must return the same object on every call (not a
    // freshly-constructed proxy).
    EXPECT_EQ(&state, &broker.hub_state());
}
