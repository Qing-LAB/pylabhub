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
    static void set_channel_closing_deadline(
        HubState &s, const std::string &name,
        std::chrono::steady_clock::time_point deadline)
    {
        s._set_channel_closing_deadline(name, deadline);
    }
    static void set_channel_zmq_node_endpoint(
        HubState &s, const std::string &name, std::string endpoint)
    {
        s._set_channel_zmq_node_endpoint(name, std::move(endpoint));
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

    // ── Schema-registry forwarders (HEP-CORE-0034 §11) ──────────────────
    static ::pylabhub::schema::SchemaRegOutcome
    on_schema_registered(HubState &s, ::pylabhub::schema::SchemaRecord rec)
    {
        return s._on_schema_registered(std::move(rec));
    }
    static std::size_t on_schemas_evicted_for_owner(HubState          &s,
                                                    const std::string &owner_uid)
    {
        return s._on_schemas_evicted_for_owner(owner_uid);
    }
    static ::pylabhub::schema::CitationOutcome validate_schema_citation(
        HubState                       &s,
        const std::string              &citer_uid,
        const std::string              &channel_producer_uid,
        const std::string              &cited_owner,
        const std::string              &cited_id,
        const std::array<uint8_t, 32>  &expected_hash,
        const std::string              &expected_packing)
    {
        return s._validate_schema_citation(citer_uid, channel_producer_uid,
                                           cited_owner, cited_id,
                                           expected_hash, expected_packing);
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
using pylabhub::hub::SchemaKey;
using pylabhub::hub::ShmBlockRef;
using pylabhub::schema::CitationOutcome;
using pylabhub::schema::SchemaRecord;
using pylabhub::schema::SchemaRegOutcome;
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

    // Per HEP-CORE-0033 §9.4, msg_type_counts is bumped at the dispatcher
    // level (broker), NOT inside HubState capability ops.  The op-only
    // L2 path here exercises the state mutation, not the wire counter.
    EXPECT_EQ(s.counters().msg_type_counts.count("REG_REQ"), 0u);

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

// HEP-CORE-0023 §2.1: Ready -> Pending demotion on heartbeat timeout.
// Role is NOT marked Disconnected — Pending means "suspicious, may recover".
// Counter bumps only when an actual Ready -> Pending transition fires.
TEST(HubStateOps, HeartbeatTimeout_DemotesChannelOnly_RoleStillConnected)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", std::chrono::steady_clock::now(), std::nullopt);
    ASSERT_EQ(s.channel("ch1")->status, ChannelStatus::Ready);
    ASSERT_EQ(s.role("prod.main.test")->state, RoleState::Connected);

    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::PendingReady);
    EXPECT_EQ(s.role("prod.main.test")->state, RoleState::Connected)
        << "Per HEP-0023 §2.1, Ready->Pending demote does NOT disconnect the role";
    EXPECT_EQ(s.counters().ready_to_pending_total, 1u);
}

// Counter must not bump when the channel is not in Ready (e.g. already
// Pending, or already gone).  Avoids over-counting under sweep races.
TEST(HubStateOps, HeartbeatTimeout_NotReady_NoCounterBump)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    // Channel is freshly registered: status=PendingReady, not Ready.
    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::PendingReady);
    EXPECT_EQ(s.counters().ready_to_pending_total, 0u);

    // Unknown channel: also no-op + no counter bump.
    HubStateTestAccess::on_heartbeat_timeout(s, "no.such.channel.uid00000001",
                                              "prod.main.test");
    EXPECT_EQ(s.counters().ready_to_pending_total, 0u);
}

// HEP-CORE-0023 §2.1: Pending -> deregistered (no grace, no Closing).
TEST(HubStateOps, PendingTimeout_DeregistersChannelAndBumpsCounter)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    ASSERT_TRUE(s.channel("ch1").has_value());
    ASSERT_EQ(s.channel("ch1")->status, ChannelStatus::PendingReady);

    HubStateTestAccess::on_pending_timeout(s, "ch1");
    EXPECT_FALSE(s.channel("ch1").has_value())
        << "Per HEP-0023 §2.1, Pending->deregistered fully removes the channel "
           "(no Closing intermediate)";
    EXPECT_EQ(s.counters().pending_to_deregistered_total, 1u);
}

// Counter must not bump when the channel is not in PendingReady state.
TEST(HubStateOps, PendingTimeout_NotPending_NoOpAndNoCounterBump)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", std::chrono::steady_clock::now(), std::nullopt);
    ASSERT_EQ(s.channel("ch1")->status, ChannelStatus::Ready);

    // Channel is Ready, not Pending — _on_pending_timeout must be no-op.
    HubStateTestAccess::on_pending_timeout(s, "ch1");
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::Ready);
    EXPECT_EQ(s.counters().pending_to_deregistered_total, 0u);

    // Unknown channel: also no-op.
    HubStateTestAccess::on_pending_timeout(s, "no.such.channel.uid00000001");
    EXPECT_EQ(s.counters().pending_to_deregistered_total, 0u);
}

// HEP-CORE-0023 §2.5: every PendingReady -> Ready transition (first heartbeat
// OR recovery from Pending) bumps `pending_to_ready_total`.
TEST(HubStateOps, Heartbeat_PendingToReady_BumpsRecoveryCounter)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    ASSERT_EQ(s.channel("ch1")->status, ChannelStatus::PendingReady);
    ASSERT_EQ(s.counters().pending_to_ready_total, 0u);

    // First heartbeat: PendingReady -> Ready (counts as "first heartbeat" recovery).
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", std::chrono::steady_clock::now(), std::nullopt);
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::Ready);
    EXPECT_EQ(s.counters().pending_to_ready_total, 1u);

    // Subsequent Ready->Ready heartbeat: no transition, no counter bump.
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", std::chrono::steady_clock::now(), std::nullopt);
    EXPECT_EQ(s.counters().pending_to_ready_total, 1u);

    // Demote, then heartbeat to recover: another Pending->Ready, counter bumps.
    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    ASSERT_EQ(s.channel("ch1")->status, ChannelStatus::PendingReady);
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", std::chrono::steady_clock::now(), std::nullopt);
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::Ready);
    EXPECT_EQ(s.counters().pending_to_ready_total, 2u);
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
    // Per HEP-CORE-0033 §9.4, msg_type counters live at dispatcher level.
    EXPECT_EQ(s.counters().msg_type_counts.count("BAND_JOIN_REQ"), 0u);
}

TEST(HubStateOps, PeerConnected_Inserts)
{
    HubState s;
    HubStateTestAccess::on_peer_connected(s, make_peer("hub.p1.test"));
    auto p = s.peer("hub.p1.test");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->state, PeerState::Connected);
    // Per HEP-CORE-0033 §9.4, msg_type counters live at dispatcher level.
    EXPECT_EQ(s.counters().msg_type_counts.count("HUB_PEER_HELLO"), 0u);
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

    // Per HEP-CORE-0033 §9.4, msg_type counters live at dispatcher level.
    EXPECT_EQ(s.counters().msg_type_counts.count("METRICS_REPORT_REQ"), 0u);
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
TEST(BrokerServicePlumbing, HubStateAccessorReturnsExternalAggregate)
{
    // HEP-CORE-0033 §4 — HubState is owned by HubHost, not by the broker.
    // Construct it externally (here, on the stack) and pass to the broker;
    // the broker stores a non-owning pointer.  In production this is what
    // `HubHost` does at startup.
    pylabhub::hub::HubState state;

    pylabhub::broker::BrokerService::Config cfg;
    cfg.endpoint  = "tcp://127.0.0.1:0";
    cfg.use_curve = false;
    pylabhub::broker::BrokerService broker(cfg, state);

    const auto &state_ref = broker.hub_state();
    auto        snap      = state_ref.snapshot();
    EXPECT_TRUE(snap.channels.empty());
    EXPECT_TRUE(snap.roles.empty());
    EXPECT_TRUE(snap.bands.empty());
    EXPECT_TRUE(snap.peers.empty());
    EXPECT_TRUE(snap.shm_blocks.empty());

    // The accessor must return the externally-owned object (same address).
    EXPECT_EQ(&state_ref, &state);
    EXPECT_EQ(&state_ref, &broker.hub_state());
}

// ═══ G2.2.1.b.1 — closing_deadline / zmq_node_endpoint primitives ══════════
//
// These primitives close the field-divergence land-mines identified in the
// G2.2.1.b code review.  Tests cover happy path, no-op-on-unknown, idempotence
// across repeated calls, and that the two primitives are independent
// (changing one doesn't disturb the other).

TEST(HubStatePrimitivesB1, SetChannelClosingDeadline_HappyPath)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    const auto t0 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    HubStateTestAccess::set_channel_closing_deadline(s, "ch1", t0);

    auto got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->closing_deadline, t0);
}

TEST(HubStatePrimitivesB1, SetChannelClosingDeadline_UnknownChannelIsNoop)
{
    HubState s;
    // No channel registered — call must not crash, must not insert.
    const auto t0 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    HubStateTestAccess::set_channel_closing_deadline(s, "nope", t0);
    EXPECT_FALSE(s.channel("nope").has_value());
}

TEST(HubStatePrimitivesB1, SetChannelClosingDeadline_OverwritesPriorValue)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    const auto t1 = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const auto t2 = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    HubStateTestAccess::set_channel_closing_deadline(s, "ch1", t1);
    HubStateTestAccess::set_channel_closing_deadline(s, "ch1", t2);
    EXPECT_EQ(s.channel("ch1")->closing_deadline, t2);
}

TEST(HubStatePrimitivesB1, SetChannelClosingDeadline_DoesNotChangeStatus)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    // Channel currently in PendingReady (default after _on_channel_registered).
    HubStateTestAccess::set_channel_closing_deadline(
        s, "ch1", std::chrono::steady_clock::now() + std::chrono::seconds(5));
    // The deadline-only primitive must NOT change status.  Caller is
    // expected to call _set_channel_status(Closing) separately.
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::PendingReady);
}

TEST(HubStatePrimitivesB1, SetChannelZmqNodeEndpoint_HappyPath)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::set_channel_zmq_node_endpoint(
        s, "ch1", "tcp://10.0.0.1:5555");
    EXPECT_EQ(s.channel("ch1")->zmq_node_endpoint, "tcp://10.0.0.1:5555");
}

TEST(HubStatePrimitivesB1, SetChannelZmqNodeEndpoint_UnknownChannelIsNoop)
{
    HubState s;
    HubStateTestAccess::set_channel_zmq_node_endpoint(s, "nope", "tcp://x:1");
    EXPECT_FALSE(s.channel("nope").has_value());
}

TEST(HubStatePrimitivesB1, SetChannelZmqNodeEndpoint_OverwritesPriorValue)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::set_channel_zmq_node_endpoint(s, "ch1", "tcp://a:1");
    HubStateTestAccess::set_channel_zmq_node_endpoint(s, "ch1", "tcp://b:2");
    EXPECT_EQ(s.channel("ch1")->zmq_node_endpoint, "tcp://b:2");
}

TEST(HubStatePrimitivesB1, SetChannelZmqNodeEndpoint_EmptyValueAccepted)
{
    // Endpoint "validation" is the broker handler's job; the primitive
    // should not editorialize.  Empty string is a meaningful sentinel
    // ("no endpoint set yet") and must round-trip.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::set_channel_zmq_node_endpoint(s, "ch1", "tcp://x:1");
    HubStateTestAccess::set_channel_zmq_node_endpoint(s, "ch1", "");
    EXPECT_EQ(s.channel("ch1")->zmq_node_endpoint, "");
}

TEST(HubStatePrimitivesB1, ClosingDeadlineAndEndpoint_AreIndependent)
{
    // Setting one primitive must not perturb the other field.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    const auto t0 = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    HubStateTestAccess::set_channel_closing_deadline(s, "ch1", t0);
    HubStateTestAccess::set_channel_zmq_node_endpoint(s, "ch1", "tcp://q:9");

    auto got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->closing_deadline,    t0);
    EXPECT_EQ(got->zmq_node_endpoint,   "tcp://q:9");

    // Mutate one — the other stays.
    HubStateTestAccess::set_channel_zmq_node_endpoint(s, "ch1", "tcp://r:10");
    auto got2 = s.channel("ch1");
    ASSERT_TRUE(got2.has_value());
    EXPECT_EQ(got2->closing_deadline,    t0);
    EXPECT_EQ(got2->zmq_node_endpoint,   "tcp://r:10");
}

TEST(HubStatePrimitivesB1, ClosingDeadline_PostChannelClose_ChannelGoneIsNoop)
{
    // After _on_channel_closed, the channel is erased from HubState.
    // Subsequent _set_channel_closing_deadline must NOT resurrect it.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_channel_closed(s, "ch1",
        ChannelCloseReason::HeartbeatTimeout);
    HubStateTestAccess::set_channel_closing_deadline(
        s, "ch1", std::chrono::steady_clock::now() + std::chrono::seconds(5));
    EXPECT_FALSE(s.channel("ch1").has_value());
}

// ═══ G2.2.1.b.1 — heartbeat: last_heartbeat updated every tick ═════════════
//
// The pre-fix behavior updated `last_heartbeat` only on the Pending→Ready
// transition (via `_set_channel_status`).  The fix routes through
// `_on_heartbeat` which updates `last_heartbeat` unconditionally.

TEST(HubStateHeartbeatB1, EveryTickUpdatesLastHeartbeat)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    auto t1 = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", t1, std::nullopt);
    EXPECT_EQ(s.channel("ch1")->last_heartbeat, t1);

    auto t2 = t1 + std::chrono::milliseconds(500);
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", t2, std::nullopt);
    EXPECT_EQ(s.channel("ch1")->last_heartbeat, t2)
        << "second heartbeat must refresh last_heartbeat";

    auto t3 = t2 + std::chrono::milliseconds(500);
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", t3, std::nullopt);
    EXPECT_EQ(s.channel("ch1")->last_heartbeat, t3);
}

TEST(HubStateHeartbeatB1, HeartbeatStatusFlowAndFireSemantics)
{
    // Status flow: PendingReady (registered) → Ready (first heartbeat) →
    // Ready (subsequent ticks; no re-fire) → PendingReady (timeout) →
    // Ready (recovery heartbeat; fires again).
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    int ready_fires = 0;
    s.subscribe_channel_status_changed(
        [&](const ChannelEntry &) { ++ready_fires; });

    auto t1 = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", t1, std::nullopt);
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::Ready);
    EXPECT_EQ(ready_fires, 1);

    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test",
                                     t1 + std::chrono::milliseconds(10),
                                     std::nullopt);
    EXPECT_EQ(ready_fires, 1) << "subsequent heartbeats must not re-fire status_changed";

    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::PendingReady);

    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test",
                                     t1 + std::chrono::seconds(5),
                                     std::nullopt);
    EXPECT_EQ(s.channel("ch1")->status, ChannelStatus::Ready);
    // Recovery via _on_heartbeat performs the same Pending→Ready
    // transition that fires status_changed.
}

TEST(HubStateHeartbeatB1, HeartbeatOnUnknownChannelDropsSilently)
{
    HubState s;
    HubStateTestAccess::on_heartbeat(s, "no.such.ch", "prod.x.y",
                                     std::chrono::steady_clock::now(),
                                     std::nullopt);
    EXPECT_FALSE(s.channel("no.such.ch").has_value());
    // Per HEP-CORE-0033 §9.4, msg_type counters are dispatcher-level
    // (not bumped by HubState ops directly).  This test exercises the
    // op-only path; the channel doesn't exist so no state mutation
    // happened — verified above.
    EXPECT_EQ(s.counters().msg_type_counts.count("HEARTBEAT_REQ"), 0u);
}

TEST(HubStateHeartbeatB1, HeartbeatRefreshesRoleLastHeartbeat)
{
    // Capability-op layered semantic: a heartbeat on the channel side
    // also bumps the role's last_heartbeat clock.  Verifies the
    // bonus role-side liveness tracking we promised in the design doc.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    auto t0 = s.role("prod.main.test")->last_heartbeat;

    // Sleep-free: stamp explicit later time to avoid wall-clock flake.
    auto t1 = t0 + std::chrono::milliseconds(100);
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", t1, std::nullopt);

    auto t_after = s.role("prod.main.test")->last_heartbeat;
    EXPECT_GE(t_after, t1)
        << "role.last_heartbeat should advance to at least the heartbeat timestamp";
}

// ────────────────────────────────────────────────────────────────────────────
// Schema-registry capability ops (HEP-CORE-0034 §11)
// ────────────────────────────────────────────────────────────────────────────

namespace
{

/// Build a SchemaRecord with deterministic content for a test.  Hash bytes
/// derive from a single seed so two records produced with the same seed
/// compare equal at the byte level.
SchemaRecord make_schema_rec(std::string owner_uid, std::string schema_id,
                             std::string packing = "aligned",
                             uint8_t     seed    = 0x11)
{
    SchemaRecord rec;
    rec.owner_uid = std::move(owner_uid);
    rec.schema_id = std::move(schema_id);
    rec.packing   = std::move(packing);
    rec.blds      = "v:f64;count:u32";
    rec.hash.fill(seed);
    return rec;
}

} // namespace

TEST(HubStateSchemas, OnSchemaRegistered_NewRecord_Created)
{
    HubState s;
    auto rec = make_schema_rec("prod.cam.uid01234567", "frame");

    auto out = HubStateTestAccess::on_schema_registered(s, rec);
    EXPECT_EQ(out, SchemaRegOutcome::kCreated);

    // Record visible via accessor.
    auto fetched = s.schema("prod.cam.uid01234567", "frame");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->owner_uid, "prod.cam.uid01234567");
    EXPECT_EQ(fetched->schema_id, "frame");
    EXPECT_EQ(fetched->packing,   "aligned");
    EXPECT_EQ(fetched->hash,      rec.hash);

    // schema_count + counter both reflect the insert.
    EXPECT_EQ(s.schema_count(), 1u);
    EXPECT_EQ(s.counters().schema_registered_total, 1u);
    EXPECT_EQ(s.counters().schema_evicted_total,    0u);
}

TEST(HubStateSchemas, OnSchemaRegistered_SameRecord_Idempotent)
{
    HubState s;
    auto rec = make_schema_rec("prod.cam.uid01234567", "frame");
    HubStateTestAccess::on_schema_registered(s, rec);

    // Second insert with identical hash + packing → idempotent, no extra
    // counter bump, single record.
    auto out2 = HubStateTestAccess::on_schema_registered(s, rec);
    EXPECT_EQ(out2, SchemaRegOutcome::kIdempotent);
    EXPECT_EQ(s.schema_count(), 1u);
    EXPECT_EQ(s.counters().schema_registered_total, 1u)
        << "idempotent re-registration must NOT bump the registered counter";
}

TEST(HubStateSchemas, OnSchemaRegistered_DifferentHash_RejectedAsHashMismatchSelf)
{
    HubState s;
    auto rec  = make_schema_rec("prod.cam.uid01234567", "frame", "aligned", 0xAA);
    auto rec2 = make_schema_rec("prod.cam.uid01234567", "frame", "aligned", 0xBB);

    HubStateTestAccess::on_schema_registered(s, rec);
    auto out = HubStateTestAccess::on_schema_registered(s, rec2);
    EXPECT_EQ(out, SchemaRegOutcome::kHashMismatchSelf);

    // Original record preserved; counter unchanged.
    auto fetched = s.schema("prod.cam.uid01234567", "frame");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->hash, rec.hash);
    EXPECT_EQ(s.counters().schema_registered_total, 1u);
}

TEST(HubStateSchemas, OnSchemaRegistered_DifferentPacking_RejectedAsHashMismatchSelf)
{
    HubState s;
    auto rec_a = make_schema_rec("prod.cam.uid01234567", "frame", "aligned", 0x11);
    auto rec_p = make_schema_rec("prod.cam.uid01234567", "frame", "packed",  0x11);
    // Same hash bytes (seed 0x11) but different packing → still rejected,
    // because in real wire flow the hash WOULD differ; this test pins the
    // store-level invariant that packing is part of the equality check
    // even when the hash bytes happen to coincide.
    HubStateTestAccess::on_schema_registered(s, rec_a);
    auto out = HubStateTestAccess::on_schema_registered(s, rec_p);
    EXPECT_EQ(out, SchemaRegOutcome::kHashMismatchSelf);
}

TEST(HubStateSchemas, ConflictPolicy_NamespaceByOwner_TwoRolesSameId)
{
    HubState s;
    // Two different producers register `frame` with different hashes.
    // HEP-CORE-0034 §8 — namespace-by-owner; both records exist
    // independently, no collision.
    auto rec_a = make_schema_rec("prod.cam_a.uid00000001", "frame", "aligned", 0xAA);
    auto rec_b = make_schema_rec("prod.cam_b.uid00000002", "frame", "aligned", 0xBB);

    EXPECT_EQ(HubStateTestAccess::on_schema_registered(s, rec_a),
              SchemaRegOutcome::kCreated);
    EXPECT_EQ(HubStateTestAccess::on_schema_registered(s, rec_b),
              SchemaRegOutcome::kCreated);

    EXPECT_EQ(s.schema_count(), 2u);
    EXPECT_TRUE(s.schema("prod.cam_a.uid00000001", "frame").has_value());
    EXPECT_TRUE(s.schema("prod.cam_b.uid00000002", "frame").has_value());
    EXPECT_EQ(s.counters().schema_registered_total, 2u);
}

TEST(HubStateSchemas, OnSchemaRegistered_EmptyOwnerOrId_ForbiddenOwner)
{
    HubState s;
    // Empty owner: rejected.
    EXPECT_EQ(HubStateTestAccess::on_schema_registered(
                  s, make_schema_rec("", "frame")),
              SchemaRegOutcome::kForbiddenOwner);
    // Empty id: rejected.
    EXPECT_EQ(HubStateTestAccess::on_schema_registered(
                  s, make_schema_rec("prod.x.uid00000001", "")),
              SchemaRegOutcome::kForbiddenOwner);
    // Empty packing: rejected (packing is part of the fingerprint).
    EXPECT_EQ(HubStateTestAccess::on_schema_registered(
                  s, make_schema_rec("prod.x.uid00000001", "frame", "")),
              SchemaRegOutcome::kForbiddenOwner);
    EXPECT_EQ(s.schema_count(), 0u);
}

TEST(HubStateSchemas, OnSchemasEvictedForOwner_RemovesAllOwnerRecords)
{
    HubState s;
    // One owner with two records, one other-owner record that must NOT
    // be evicted.
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("prod.cam.uid01234567", "frame_v1", "aligned", 0xAA));
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("prod.cam.uid01234567", "frame_v2", "aligned", 0xBB));
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("prod.other.uid89abcdef", "frame", "aligned", 0xCC));
    ASSERT_EQ(s.schema_count(), 3u);

    auto removed = HubStateTestAccess::on_schemas_evicted_for_owner(
        s, "prod.cam.uid01234567");
    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(s.schema_count(), 1u);
    EXPECT_TRUE(s.schema("prod.other.uid89abcdef", "frame").has_value());
    EXPECT_FALSE(s.schema("prod.cam.uid01234567", "frame_v1").has_value());
    EXPECT_FALSE(s.schema("prod.cam.uid01234567", "frame_v2").has_value());

    EXPECT_EQ(s.counters().schema_evicted_total, 2u);
}

TEST(HubStateSchemas, OnSchemasEvictedForOwner_HubGlobalNeverEvicted)
{
    HubState s;
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("hub", "lab.demo.frame@1"));

    auto removed = HubStateTestAccess::on_schemas_evicted_for_owner(s, "hub");
    EXPECT_EQ(removed, 0u);
    EXPECT_EQ(s.schema_count(), 1u);
    EXPECT_TRUE(s.schema("hub", "lab.demo.frame@1").has_value());
}

TEST(HubStateSchemas, RoleDisconnect_CascadeEvictsOwnedSchemas)
{
    // HEP-CORE-0034 §7.2 — schemas evict atomically with role state
    // transition to Disconnected.  Producer's role record is registered;
    // its schemas are too; on _set_role_disconnected the schemas vanish.
    HubState s;

    HubStateTestAccess::set_role_registered(s, make_role("prod.cam.uid01234567"));
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("prod.cam.uid01234567", "frame", "aligned", 0xAA));
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("prod.cam.uid01234567", "inbox", "aligned", 0xBB));
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("hub", "lab.demo.frame@1"));
    ASSERT_EQ(s.schema_count(), 3u);

    HubStateTestAccess::set_role_disconnected(s, "prod.cam.uid01234567");

    EXPECT_EQ(s.schema_count(), 1u) << "hub-global must survive";
    EXPECT_TRUE(s.schema("hub", "lab.demo.frame@1").has_value());
    EXPECT_FALSE(s.schema("prod.cam.uid01234567", "frame").has_value());
    EXPECT_FALSE(s.schema("prod.cam.uid01234567", "inbox").has_value());

    // The cascade went through _set_role_disconnected, which adds 2 to
    // schema_evicted_total (one per record removed).
    EXPECT_EQ(s.counters().schema_evicted_total, 2u);
}

TEST(HubStateSchemas, ValidateCitation_SelfOwnedRecord_Ok)
{
    HubState s;
    const std::string prod_uid = "prod.cam.uid01234567";
    HubStateTestAccess::set_role_registered(s, make_role(prod_uid));
    auto rec = make_schema_rec(prod_uid, "frame", "aligned", 0x11);
    HubStateTestAccess::on_schema_registered(s, rec);

    auto out = HubStateTestAccess::validate_schema_citation(
        s, /*citer_uid=*/"cons.viewer.uid00000005",
        /*channel_producer_uid=*/prod_uid,
        /*cited_owner=*/prod_uid,
        /*cited_id=*/"frame",
        /*expected_hash=*/rec.hash,
        /*expected_packing=*/"aligned");
    EXPECT_TRUE(out.ok()) << "self-owned-record citation should validate; reason=" << out.detail;
    EXPECT_EQ(s.counters().schema_citation_rejected_total, 0u);
}

TEST(HubStateSchemas, ValidateCitation_HubGlobal_Ok)
{
    HubState s;
    auto rec = make_schema_rec("hub", "lab.demo.frame@1", "aligned", 0x33);
    HubStateTestAccess::on_schema_registered(s, rec);

    auto out = HubStateTestAccess::validate_schema_citation(
        s, /*citer_uid=*/"prod.cam.uid01234567",
        /*channel_producer_uid=*/"prod.cam.uid01234567",
        /*cited_owner=*/"hub",
        /*cited_id=*/"lab.demo.frame@1",
        /*expected_hash=*/rec.hash,
        /*expected_packing=*/"aligned");
    EXPECT_TRUE(out.ok()) << "hub-global citation should validate";
    EXPECT_EQ(s.counters().schema_citation_rejected_total, 0u);
}

TEST(HubStateSchemas, ValidateCitation_CrossCitation_Rejected)
{
    // Consumer connects to producer A but cites producer B's schema.
    // HEP-CORE-0034 §9.1 — rejected even if hashes match.
    HubState s;
    const std::string prod_a = "prod.cam_a.uid00000001";
    const std::string prod_b = "prod.cam_b.uid00000002";

    HubStateTestAccess::set_role_registered(s, make_role(prod_a));
    HubStateTestAccess::set_role_registered(s, make_role(prod_b));

    auto rec_a = make_schema_rec(prod_a, "frame", "aligned", 0x11);
    auto rec_b = make_schema_rec(prod_b, "frame", "aligned", 0x11);  // same hash bytes
    HubStateTestAccess::on_schema_registered(s, rec_a);
    HubStateTestAccess::on_schema_registered(s, rec_b);

    auto out = HubStateTestAccess::validate_schema_citation(
        s, /*citer_uid=*/"cons.viewer.uid00000005",
        /*channel_producer_uid=*/prod_a,           // connecting to A
        /*cited_owner=*/prod_b,                    // citing B → rejected
        /*cited_id=*/"frame",
        /*expected_hash=*/rec_b.hash,              // even though hashes match!
        /*expected_packing=*/"aligned");
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.reason, CitationOutcome::Reason::kCrossCitation);
    EXPECT_EQ(s.counters().schema_citation_rejected_total, 1u);
}

TEST(HubStateSchemas, ValidateCitation_FingerprintMismatch_Rejected)
{
    HubState s;
    const std::string prod_uid = "prod.cam.uid01234567";
    HubStateTestAccess::set_role_registered(s, make_role(prod_uid));

    auto rec = make_schema_rec(prod_uid, "frame", "aligned", 0x11);
    HubStateTestAccess::on_schema_registered(s, rec);

    // Hash mismatch:
    std::array<uint8_t, 32> wrong_hash;
    wrong_hash.fill(0x99);
    auto out_h = HubStateTestAccess::validate_schema_citation(
        s, "cons.viewer.uid00000005", prod_uid, prod_uid, "frame",
        wrong_hash, "aligned");
    EXPECT_FALSE(out_h.ok());
    EXPECT_EQ(out_h.reason, CitationOutcome::Reason::kFingerprintMismatch);

    // Packing mismatch:
    auto out_p = HubStateTestAccess::validate_schema_citation(
        s, "cons.viewer.uid00000005", prod_uid, prod_uid, "frame",
        rec.hash, "packed");
    EXPECT_FALSE(out_p.ok());
    EXPECT_EQ(out_p.reason, CitationOutcome::Reason::kFingerprintMismatch);

    EXPECT_EQ(s.counters().schema_citation_rejected_total, 2u);
}

TEST(HubStateSchemas, ValidateCitation_UnknownSchema_Rejected)
{
    HubState s;
    const std::string prod_uid = "prod.cam.uid01234567";
    HubStateTestAccess::set_role_registered(s, make_role(prod_uid));
    // No schema registered.

    std::array<uint8_t, 32> any_hash{};
    auto out = HubStateTestAccess::validate_schema_citation(
        s, "cons.viewer.uid00000005", prod_uid, prod_uid, "frame",
        any_hash, "aligned");
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.reason, CitationOutcome::Reason::kUnknownSchema);
    EXPECT_EQ(s.counters().schema_citation_rejected_total, 1u);
}

TEST(HubStateSchemas, ValidateCitation_UnknownOwner_Rejected)
{
    HubState s;
    // No role registered; cited owner is neither hub nor a registered role.
    const std::string ghost = "prod.ghost.uid00000099";
    std::array<uint8_t, 32> any_hash{};
    auto out = HubStateTestAccess::validate_schema_citation(
        s, "cons.viewer.uid00000005", ghost, ghost, "frame",
        any_hash, "aligned");
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.reason, CitationOutcome::Reason::kUnknownOwner);
    EXPECT_EQ(s.counters().schema_citation_rejected_total, 1u);
}
