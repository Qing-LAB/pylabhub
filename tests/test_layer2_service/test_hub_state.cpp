/**
 * @file test_hub_state.cpp
 * @brief L2 unit tests for HubState (HEP-0033 §G2 + HEP-CORE-0023 §2 +
 *        HEP-CORE-0034 schema registry).
 *
 * Covers the post-M1.2 contract: per-presence FSM is the single source of
 * truth; channel state surfaces only as the derived `ChannelObservable`
 * (HEP-CORE-0023 §2.2).  The legacy `ChannelEntry.status`,
 * `ChannelEntry.last_heartbeat`, `RoleEntry.last_heartbeat`, and the
 * grace/closing/FORCE_SHUTDOWN paths have been retired — tests for them
 * were removed alongside the production code.
 */

#include "utils/broker_service.hpp"
#include "utils/hub_state.hpp"
#include "hub_state_test_access.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using pylabhub::hub::BandEntry;
using pylabhub::hub::BandMember;
using pylabhub::hub::ChannelCloseReason;
using pylabhub::hub::ChannelEntry;
using pylabhub::hub::ChannelObservable;
using pylabhub::hub::ConsumerEntry;
using pylabhub::hub::ProducerEntry;
using pylabhub::hub::HandlerId;
using pylabhub::hub::HubState;
using pylabhub::hub::HubStateSnapshot;
using pylabhub::hub::kInvalidHandlerId;
using pylabhub::hub::PeerEntry;
using pylabhub::hub::PeerState;
using pylabhub::hub::RoleEntry;
using pylabhub::hub::RolePresence;
using pylabhub::hub::RoleState;
using pylabhub::hub::SchemaKey;
using pylabhub::hub::ShmBlockRef;
using pylabhub::hub::observe_channel;
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
    e.name           = name;
    e.shm_name       = name + "-shm";
    e.schema_hash    = std::string(64, 'a');
    e.schema_version = 1;
    ProducerEntry p;
    p.producer_pid = 4242;
    p.role_uid     = "prod.main.test";
    p.role_name    = "main";
    e.producers.push_back(std::move(p));
    return e;
}

ConsumerEntry make_consumer(const std::string &uid, uint64_t pid = 1234)
{
    ConsumerEntry c;
    c.consumer_pid = pid;
    c.role_uid     = uid;
    c.role_name    = uid;
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
    p.endpoint = "tcp://hub.test:5570";
    return p;
}

ChannelObservable channel_observable(const HubState &s, const std::string &name)
{
    auto snap = s.snapshot();
    auto cit  = snap.channels.find(name);
    if (cit == snap.channels.end()) return ChannelObservable::kAbsent;
    return observe_channel(cit->second, snap);
}

// Look up a presence row inside an already-captured snapshot.  Callers
// MUST hold the snapshot in scope for the lifetime of the returned
// pointer — the snapshot owns the underlying RoleEntry.
const RolePresence *
find_presence_in(const HubStateSnapshot &snap, const std::string &channel,
                 const std::string &uid, const std::string &role_type)
{
    auto rit = snap.roles.find(uid);
    if (rit == snap.roles.end()) return nullptr;
    return rit->second.find_presence(channel, role_type);
}

const RolePresence *
find_producer_in(const HubStateSnapshot &snap, const std::string &channel,
                 const std::string &uid)
{
    return find_presence_in(snap, channel, uid, "producer");
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
    // Newly opened with no producer-presence yet → kAbsent.  After the
    // producer's RoleEntry + presence row land via _on_channel_registered
    // the observable becomes kRegistering.  Tested in HubStateOps below.
    EXPECT_EQ(channel_observable(s, "ch1"), ChannelObservable::kAbsent);

    ASSERT_EQ(fired.size(), 1u);
    EXPECT_EQ(fired[0], "ch1");

    auto snap = s.snapshot();
    EXPECT_EQ(snap.channels.count("ch1"), 1u);
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
    EXPECT_EQ(r->uid, "prod.r1.test");
}

TEST(HubStateSkeleton, RoleDisconnected_FiresOnceThenEntryGone)
{
    // Wave M3 step 4 contract (2026-05-11): _set_role_disconnected is
    // TERMINAL cleanup — fires the role_disconnected handler exactly
    // once and ERASES the RoleEntry from pImpl->roles.  Second call
    // finds no entry and returns without firing (idempotent by
    // construction — the entry being gone IS the memoization,
    // retiring the previous `disconnected_fired` PATCH).
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    int fired = 0;
    s.subscribe_role_disconnected([&](const std::string &uid) {
        EXPECT_EQ(uid, "prod.main.test");
        ++fired;
    });

    // Role + presence row exist before disconnect.
    ASSERT_TRUE(s.role("prod.main.test").has_value());

    HubStateTestAccess::set_role_disconnected(s, "prod.main.test");
    HubStateTestAccess::set_role_disconnected(s, "prod.main.test");
    EXPECT_EQ(fired, 1);

    // Role entry is now ERASED.  Snapshot lookup returns nullopt;
    // find_producer_in returns nullptr (no role in snap.roles).
    EXPECT_FALSE(s.role("prod.main.test").has_value());
    auto snap = s.snapshot();
    EXPECT_EQ(find_producer_in(snap, "ch1", "prod.main.test"), nullptr);
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

    s.unsubscribe(id);
    s.unsubscribe(kInvalidHandlerId);
    s.unsubscribe(999999);
}

// ─── Concurrency sanity ─────────────────────────────────────────────────────

TEST(HubStateSkeleton, ConcurrentReadersDoNotRaceWithSingleWriter)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    std::atomic<bool> stop{false};
    std::atomic<int>  readers_ready{0};
    constexpr int     kReaderCount = 4;

    std::vector<std::thread> readers;
    for (int t = 0; t < kReaderCount; ++t)
    {
        readers.emplace_back([&] {
            readers_ready.fetch_add(1);
            while (!stop.load())
            {
                auto snap = s.snapshot();
                (void)snap.channels.size();
                (void)snap.counters.msg_type_counts.size();
            }
        });
    }

    while (readers_ready.load() < kReaderCount) std::this_thread::yield();

    std::thread writer([&] {
        for (int i = 0; i < 2000 && !stop.load(); ++i)
        {
            HubStateTestAccess::on_heartbeat(
                s, "ch1", "prod.main.test", "producer",
                std::chrono::steady_clock::now(), std::nullopt);
            HubStateTestAccess::bump_counter(s, "tick");
        }
        stop.store(true);
    });

    writer.join();
    for (auto &r : readers) r.join();
    // ASan/TSan catches the data-race contract directly.
}

// ═══ Capability-operation layer (HEP-0033 §G2 + HEP-CORE-0023) ══════════════

TEST(HubStateOps, ChannelRegistered_ComposesChannelAndRoleAndShmAndCounter)
{
    HubState s;

    std::vector<std::string> opened_fired;
    std::vector<std::string> role_fired;
    s.subscribe_channel_opened(
        [&](const ChannelEntry &e) { opened_fired.push_back(e.name); });
    s.subscribe_role_registered(
        [&](const RoleEntry &r) { role_fired.push_back(r.uid); });

    auto ch              = make_channel("ch1");
    ch.has_shared_memory = true;
    // Wave M2.5 step 6.5: zmq_pubkey is per-producer (HEP-CORE-0021
    // §5.2), not channel-scope.  The legacy _on_channel_registered
    // test-only path now reads from the FIRST producer's pubkey.
    ASSERT_FALSE(ch.producers.empty());
    ch.producers.front().zmq_pubkey = "pubkey-xyz";
    HubStateTestAccess::on_channel_registered(s, ch);

    auto got = s.channel("ch1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "ch1");

    auto r = s.role("prod.main.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->role_tag,   "prod");
    EXPECT_EQ(r->name,       "main");
    EXPECT_EQ(r->pubkey_z85, "pubkey-xyz");
    ASSERT_EQ(r->channels.size(), 1u);
    EXPECT_EQ(r->channels[0], "ch1");

    // HEP-CORE-0023 §2.6 — eager presence creation on REG.  Producer-
    // presence exists in Connected state with first_heartbeat_seen=false,
    // so the channel observable is kRegistering.
    const auto *p = r->find_presence("ch1", "producer");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->state, RoleState::Connected);
    EXPECT_FALSE(p->first_heartbeat_seen);
    EXPECT_EQ(channel_observable(s, "ch1"), ChannelObservable::kRegistering);

    auto shm = s.shm_block("ch1");
    ASSERT_TRUE(shm.has_value());
    EXPECT_EQ(shm->block_path, "ch1-shm");

    EXPECT_EQ(s.counters().msg_type_counts.count("REG_REQ"), 0u);
    EXPECT_EQ(opened_fired, (std::vector<std::string>{"ch1"}));
    EXPECT_EQ(role_fired, (std::vector<std::string>{"prod.main.test"}));
}

TEST(HubStateOps, ChannelRegistered_NoShm_SkipsShmBlock)
{
    HubState s;
    auto     ch         = make_channel("ch1");
    ch.has_shared_memory = false;
    HubStateTestAccess::on_channel_registered(s, ch);
    EXPECT_FALSE(s.shm_block("ch1").has_value());
}

TEST(HubStateOps, ChannelRegistered_SameProducerOnTwoChannels_MergesRoleChannels)
{
    HubState s;
    auto     c1 = make_channel("ch1");
    auto     c2 = make_channel("ch2");
    HubStateTestAccess::on_channel_registered(s, c1);
    HubStateTestAccess::on_channel_registered(s, c2);

    auto r = s.role("prod.main.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->channels.size(), 2u);
    // Two presence rows — one per channel.
    ASSERT_NE(r->find_presence("ch1", "producer"), nullptr);
    ASSERT_NE(r->find_presence("ch2", "producer"), nullptr);
}

TEST(HubStateOps, ChannelRegistered_EmptyProducerUid_SkipsRole)
{
    HubState s;
    auto     ch = make_channel("ch1");
    ch.producers.clear();  // No producers admitted — exercise the empty-producers path.
    HubStateTestAccess::on_channel_registered(s, ch);
    EXPECT_TRUE(s.channel("ch1").has_value());
    EXPECT_TRUE(s.snapshot().roles.empty());
}

TEST(HubStateOps, ChannelClosed_RemovesChannelAndScrubsRole)
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
    // HEP-CORE-0023 §2.1 atomic teardown — producer-presence on the
    // closed channel transitions Disconnected.
    const auto *p = r->find_presence("ch1", "producer");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->state, RoleState::Disconnected);

    EXPECT_EQ(s.counters().msg_type_counts.at("close:VoluntaryDereg"), 1u);
}

TEST(HubStateOps, ChannelClosed_MultiChannel_LeavesOtherPresences)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_channel_registered(s, make_channel("ch2"));
    ASSERT_EQ(s.role("prod.main.test")->channels.size(), 2u);

    HubStateTestAccess::on_channel_closed(
        s, "ch1", ChannelCloseReason::VoluntaryDereg);
    EXPECT_FALSE(s.channel("ch1").has_value());
    EXPECT_TRUE(s.channel("ch2").has_value());

    auto r = s.role("prod.main.test");
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->channels.size(), 1u);
    EXPECT_EQ(r->channels[0], "ch2");
    // ch1 presence Disconnected, ch2 presence still Connected.
    const auto *p1 = r->find_presence("ch1", "producer");
    const auto *p2 = r->find_presence("ch2", "producer");
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p1->state, RoleState::Disconnected);
    EXPECT_EQ(p2->state, RoleState::Connected);
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
    EXPECT_EQ(r->name,     "A");
    EXPECT_EQ(r->channels.size(), 1u);
    EXPECT_EQ(r->channels[0], "ch1");
    // Consumer-presence created at REG time per HEP-CORE-0023 §2.6.
    ASSERT_NE(r->find_presence("ch1", "consumer"), nullptr);
}

TEST(HubStateOps, ConsumerLeft_RemovesFromChannelAndDisconnectsPresence)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_consumer_joined(s, "ch1", make_consumer("cons.A.test"));
    HubStateTestAccess::on_consumer_left(s, "ch1", "cons.A.test");

    auto ch = s.channel("ch1");
    ASSERT_TRUE(ch.has_value());
    EXPECT_TRUE(ch->consumers.empty());

    auto r = s.role("cons.A.test");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->channels.empty());
    const auto *p = r->find_presence("ch1", "consumer");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->state, RoleState::Disconnected);
}

TEST(HubStateOps, Heartbeat_FirstTickFlipsObservableToLive)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    ASSERT_EQ(channel_observable(s, "ch1"), ChannelObservable::kRegistering);

    int fires = 0;
    s.subscribe_channel_status_changed([&](const ChannelEntry &e,
                                            ChannelObservable obs) {
        EXPECT_EQ(e.name, "ch1");
        EXPECT_EQ(obs, ChannelObservable::kLive);
        ++fires;
    });

    const auto t1 = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", "producer",
                                     t1, std::nullopt);
    EXPECT_EQ(fires, 1);
    EXPECT_EQ(channel_observable(s, "ch1"), ChannelObservable::kLive);

    // Subsequent tick: same observable → no re-fire.
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", "producer",
                                     t1 + std::chrono::milliseconds(10),
                                     std::nullopt);
    EXPECT_EQ(fires, 1);
}

TEST(HubStateOps, Heartbeat_WithMetrics_StoresOnPresence)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    nlohmann::json metrics = {{"qps", 123}, {"errors", 0}};
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", "producer",
                                     std::chrono::steady_clock::now(), metrics);

    auto snap = s.snapshot();
    const auto *p = find_producer_in(snap, "ch1", "prod.main.test");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->latest_metrics, metrics);
    EXPECT_NE(p->metrics_collected_at,
              std::chrono::system_clock::time_point{});
}

TEST(HubStateOps, HeartbeatTimeout_DemotesProducerPresence)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", "producer",
        std::chrono::steady_clock::now(), std::nullopt);
    ASSERT_EQ(channel_observable(s, "ch1"), ChannelObservable::kLive);

    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    EXPECT_EQ(channel_observable(s, "ch1"), ChannelObservable::kStalled);
    auto snap = s.snapshot();
    const auto *p = find_producer_in(snap, "ch1", "prod.main.test");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->state, RoleState::Pending);
    EXPECT_EQ(s.counters().ready_to_pending_total, 1u);
}

TEST(HubStateOps, HeartbeatTimeout_AlreadyPending_NoDoubleCounter)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    // Eager presence: Connected + first_heartbeat_seen=false.
    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    EXPECT_EQ(s.counters().ready_to_pending_total, 1u);

    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    EXPECT_EQ(s.counters().ready_to_pending_total, 1u)
        << "Pending → Pending must not double-bump the counter";

    HubStateTestAccess::on_heartbeat_timeout(s, "no.such.channel.uid00000001",
                                              "prod.main.test");
    EXPECT_EQ(s.counters().ready_to_pending_total, 1u);
}

TEST(HubStateOps, PendingTimeout_AtomicallyTearsDownChannel)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    // Force producer-presence to Pending via heartbeat timeout.
    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    {
        auto snap = s.snapshot();
        ASSERT_EQ(find_producer_in(snap, "ch1", "prod.main.test")->state,
                  RoleState::Pending);
    }

    auto pt = HubStateTestAccess::on_pending_timeout(s, "ch1", "prod.main.test");
    EXPECT_TRUE(pt.removed);
    EXPECT_TRUE(pt.channel_now_empty);
    EXPECT_FALSE(s.channel("ch1").has_value())
        << "HEP-CORE-0023 §2.1 atomic teardown — Pending->Disconnected "
           "removes the channel in the same handler";
    EXPECT_EQ(s.counters().pending_to_deregistered_total, 1u);
}

TEST(HubStateOps, PendingTimeout_NotPending_NoOpAndNoCounterBump)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", "producer",
        std::chrono::steady_clock::now(), std::nullopt);
    {
        auto snap = s.snapshot();
        ASSERT_EQ(find_producer_in(snap, "ch1", "prod.main.test")->state,
                  RoleState::Connected);
    }

    // Not-Pending presence → no-op; counter unchanged.
    auto pt = HubStateTestAccess::on_pending_timeout(s, "ch1", "prod.main.test");
    EXPECT_FALSE(pt.removed);
    EXPECT_FALSE(pt.channel_now_empty);
    EXPECT_TRUE(s.channel("ch1").has_value());
    EXPECT_EQ(s.counters().pending_to_deregistered_total, 0u);

    // Unknown channel → no-op.
    auto pt2 = HubStateTestAccess::on_pending_timeout(
        s, "no.such.channel.uid00000001", "prod.main.test");
    EXPECT_FALSE(pt2.removed);
    EXPECT_EQ(s.counters().pending_to_deregistered_total, 0u);
}

TEST(HubStateOps, Heartbeat_PendingToConnected_BumpsRecoveryCounter)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    ASSERT_EQ(s.counters().pending_to_ready_total, 0u);

    // First heartbeat: kRegistering → kLive (was_first==true) — counts.
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", "producer",
        std::chrono::steady_clock::now(), std::nullopt);
    EXPECT_EQ(channel_observable(s, "ch1"), ChannelObservable::kLive);
    // Note: pending_to_ready bumps only on Pending→Connected recovery,
    // not on first-heartbeat.  Verified next.

    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", "producer",
        std::chrono::steady_clock::now(), std::nullopt);
    EXPECT_EQ(s.counters().pending_to_ready_total, 0u);

    // Demote, then heartbeat to recover — Pending→Connected.
    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    {
        auto snap = s.snapshot();
        ASSERT_EQ(find_producer_in(snap, "ch1", "prod.main.test")->state,
                  RoleState::Pending);
    }
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", "producer",
        std::chrono::steady_clock::now(), std::nullopt);
    EXPECT_EQ(channel_observable(s, "ch1"), ChannelObservable::kLive);
    EXPECT_EQ(s.counters().pending_to_ready_total, 1u);
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
    EXPECT_EQ(r->name, "r1");
    EXPECT_EQ(r->role_tag, "prod");
    EXPECT_TRUE(r->channels.empty());
    EXPECT_EQ(s.counters().msg_type_counts.count("BAND_JOIN_REQ"), 0u);
}

TEST(HubStateOps, PeerConnected_Inserts)
{
    HubState s;
    HubStateTestAccess::on_peer_connected(s, make_peer("hub.p1.test"));
    auto p = s.peer("hub.p1.test");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->state, PeerState::Connected);
    EXPECT_EQ(s.counters().msg_type_counts.count("HUB_PEER_HELLO"), 0u);
}

TEST(HubStateOps, MetricsReported_StoresOnPresenceWithoutLivenessSideEffect)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    const auto hb_time = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(
        s, "ch1", "prod.main.test", "producer", hb_time, std::nullopt);
    std::chrono::steady_clock::time_point presence_hb_before;
    {
        auto snap = s.snapshot();
        presence_hb_before =
            find_producer_in(snap, "ch1", "prod.main.test")->last_heartbeat;
    }

    nlohmann::json m   = {{"rx_count", 42}};
    const auto     now = std::chrono::system_clock::now();
    HubStateTestAccess::on_metrics_reported(s, "ch1", "prod.main.test", m, now);

    auto snap = s.snapshot();
    const auto *p = find_producer_in(snap, "ch1", "prod.main.test");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->latest_metrics, m);
    EXPECT_EQ(p->metrics_collected_at, now);
    // Metrics report must NOT advance the presence's last_heartbeat.
    EXPECT_EQ(p->last_heartbeat, presence_hb_before);
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

// ═══ Validator wiring (HEP-0033 §G2.2.0b) ═══════════════════════════════════

TEST(HubStateValidation, ChannelRegistered_InvalidChannelName_DroppedAndCounted)
{
    HubState s;
    ChannelEntry bad = make_channel("prod.oops"); // reserved first component
    HubStateTestAccess::on_channel_registered(s, bad);

    EXPECT_FALSE(s.channel("prod.oops").has_value());
    EXPECT_TRUE(s.snapshot().roles.empty());
    EXPECT_TRUE(s.snapshot().shm_blocks.empty());

    const auto &counts = s.counters().msg_type_counts;
    auto it = counts.find("sys.invalid_identifier_rejected");
    ASSERT_NE(it, counts.end());
    EXPECT_EQ(it->second, 1u);
    EXPECT_EQ(counts.count("REG_REQ"), 0u);
}

TEST(HubStateValidation, ConsumerJoined_InvalidRoleUid_DroppedAndCounted)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    ConsumerEntry c;
    c.consumer_pid = 1;
    c.role_uid     = "bogus-uid"; // missing tag prefix
    HubStateTestAccess::on_consumer_joined(s, "ch1", c);

    auto ch = s.channel("ch1");
    ASSERT_TRUE(ch.has_value());
    EXPECT_TRUE(ch->consumers.empty());
    EXPECT_FALSE(s.role("bogus-uid").has_value());

    const auto &counts = s.counters().msg_type_counts;
    EXPECT_EQ(counts.at("sys.invalid_identifier_rejected"), 1u);
    EXPECT_EQ(counts.count("CONSUMER_REG_REQ"), 0u);
}

// ═══ BrokerService plumbing ═════════════════════════════════════════════════

TEST(BrokerServicePlumbing, HubStateAccessorReturnsExternalAggregate)
{
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

    EXPECT_EQ(&state_ref, &state);
    EXPECT_EQ(&state_ref, &broker.hub_state());
}

// ═══ Heartbeat/observable contract ══════════════════════════════════════════

TEST(HubStateHeartbeat, EveryTickUpdatesPresenceLastHeartbeat)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    auto t1 = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", "producer",
                                     t1, std::nullopt);
    {
        auto snap = s.snapshot();
        EXPECT_EQ(find_producer_in(snap, "ch1", "prod.main.test")->last_heartbeat, t1);
    }

    auto t2 = t1 + std::chrono::milliseconds(500);
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", "producer",
                                     t2, std::nullopt);
    {
        auto snap = s.snapshot();
        EXPECT_EQ(find_producer_in(snap, "ch1", "prod.main.test")->last_heartbeat, t2);
    }
}

TEST(HubStateHeartbeat, ObservableFlowAndFireSemantics)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));

    int fires = 0;
    s.subscribe_channel_status_changed(
        [&](const ChannelEntry &, ChannelObservable) { ++fires; });

    auto t1 = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", "producer",
                                     t1, std::nullopt);
    EXPECT_EQ(channel_observable(s, "ch1"), ChannelObservable::kLive);
    EXPECT_EQ(fires, 1);

    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", "producer",
                                     t1 + std::chrono::milliseconds(10),
                                     std::nullopt);
    EXPECT_EQ(fires, 1)
        << "subsequent heartbeats must not re-fire status_changed";

    HubStateTestAccess::on_heartbeat_timeout(s, "ch1", "prod.main.test");
    EXPECT_EQ(channel_observable(s, "ch1"), ChannelObservable::kStalled);

    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", "producer",
                                     t1 + std::chrono::seconds(5),
                                     std::nullopt);
    EXPECT_EQ(channel_observable(s, "ch1"), ChannelObservable::kLive);
}

TEST(HubStateHeartbeat, HeartbeatOnUnknownPresenceIsNoop)
{
    HubState s;
    HubStateTestAccess::on_heartbeat(s, "no.such.ch", "prod.x.y", "producer",
                                     std::chrono::steady_clock::now(),
                                     std::nullopt);
    EXPECT_FALSE(s.channel("no.such.ch").has_value());
    EXPECT_EQ(s.counters().msg_type_counts.count("HEARTBEAT_REQ"), 0u);
}

TEST(HubStateHeartbeat, ConsumerHeartbeatDoesNotRefreshProducerPresence)
{
    // HEP-CORE-0019 §2.3 — per-presence keying.  A consumer heartbeat
    // refreshes ONLY the consumer-presence row, never the producer's.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_consumer_joined(s, "ch1",
                                            make_consumer("cons.A.test"));

    auto t1 = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(s, "ch1", "prod.main.test", "producer",
                                     t1, std::nullopt);
    std::chrono::steady_clock::time_point producer_t1;
    {
        auto snap = s.snapshot();
        producer_t1 =
            find_producer_in(snap, "ch1", "prod.main.test")->last_heartbeat;
    }

    auto t2 = t1 + std::chrono::seconds(1);
    HubStateTestAccess::on_heartbeat(s, "ch1", "cons.A.test", "consumer",
                                     t2, std::nullopt);

    auto snap = s.snapshot();
    auto producer_after =
        find_producer_in(snap, "ch1", "prod.main.test")->last_heartbeat;
    EXPECT_EQ(producer_after, producer_t1)
        << "consumer heartbeat must not refresh producer-presence";
}

// ─── Schema-registry capability ops (HEP-CORE-0034 §11) ─────────────────────

namespace
{

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

    auto fetched = s.schema("prod.cam.uid01234567", "frame");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->owner_uid, "prod.cam.uid01234567");
    EXPECT_EQ(fetched->schema_id, "frame");
    EXPECT_EQ(fetched->packing,   "aligned");
    EXPECT_EQ(fetched->hash,      rec.hash);

    EXPECT_EQ(s.schema_count(), 1u);
    EXPECT_EQ(s.counters().schema_registered_total, 1u);
    EXPECT_EQ(s.counters().schema_evicted_total,    0u);
}

TEST(HubStateSchemas, OnSchemaRegistered_SameRecord_Idempotent)
{
    HubState s;
    auto rec = make_schema_rec("prod.cam.uid01234567", "frame");
    HubStateTestAccess::on_schema_registered(s, rec);

    auto out2 = HubStateTestAccess::on_schema_registered(s, rec);
    EXPECT_EQ(out2, SchemaRegOutcome::kIdempotent);
    EXPECT_EQ(s.schema_count(), 1u);
    EXPECT_EQ(s.counters().schema_registered_total, 1u);
}

TEST(HubStateSchemas, OnSchemaRegistered_DifferentHash_RejectedAsHashMismatchSelf)
{
    HubState s;
    auto rec  = make_schema_rec("prod.cam.uid01234567", "frame", "aligned", 0xAA);
    auto rec2 = make_schema_rec("prod.cam.uid01234567", "frame", "aligned", 0xBB);

    HubStateTestAccess::on_schema_registered(s, rec);
    auto out = HubStateTestAccess::on_schema_registered(s, rec2);
    EXPECT_EQ(out, SchemaRegOutcome::kHashMismatchSelf);

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
    HubStateTestAccess::on_schema_registered(s, rec_a);
    auto out = HubStateTestAccess::on_schema_registered(s, rec_p);
    EXPECT_EQ(out, SchemaRegOutcome::kHashMismatchSelf);
}

TEST(HubStateSchemas, ConflictPolicy_NamespaceByOwner_TwoRolesSameId)
{
    HubState s;
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
    EXPECT_EQ(HubStateTestAccess::on_schema_registered(
                  s, make_schema_rec("", "frame")),
              SchemaRegOutcome::kForbiddenOwner);
    EXPECT_EQ(HubStateTestAccess::on_schema_registered(
                  s, make_schema_rec("prod.x.uid00000001", "")),
              SchemaRegOutcome::kForbiddenOwner);
    EXPECT_EQ(HubStateTestAccess::on_schema_registered(
                  s, make_schema_rec("prod.x.uid00000001", "frame", "")),
              SchemaRegOutcome::kForbiddenOwner);
    EXPECT_EQ(s.schema_count(), 0u);
}

TEST(HubStateSchemas, OnSchemasEvictedForOwner_RemovesAllOwnerRecords)
{
    HubState s;
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
        s, "cons.viewer.uid00000005", prod_uid, prod_uid, "frame",
        rec.hash, "aligned");
    EXPECT_TRUE(out.ok()) << "self-owned-record citation should validate; reason=" << out.detail;
    EXPECT_EQ(s.counters().schema_citation_rejected_total, 0u);
}

TEST(HubStateSchemas, ValidateCitation_HubGlobal_Ok)
{
    HubState s;
    auto rec = make_schema_rec("hub", "lab.demo.frame@1", "aligned", 0x33);
    HubStateTestAccess::on_schema_registered(s, rec);

    auto out = HubStateTestAccess::validate_schema_citation(
        s, "prod.cam.uid01234567", "prod.cam.uid01234567", "hub",
        "lab.demo.frame@1", rec.hash, "aligned");
    EXPECT_TRUE(out.ok()) << "hub-global citation should validate";
    EXPECT_EQ(s.counters().schema_citation_rejected_total, 0u);
}

TEST(HubStateSchemas, ValidateCitation_CrossCitation_Rejected)
{
    HubState s;
    const std::string prod_a = "prod.cam_a.uid00000001";
    const std::string prod_b = "prod.cam_b.uid00000002";

    HubStateTestAccess::set_role_registered(s, make_role(prod_a));
    HubStateTestAccess::set_role_registered(s, make_role(prod_b));

    auto rec_a = make_schema_rec(prod_a, "frame", "aligned", 0x11);
    auto rec_b = make_schema_rec(prod_b, "frame", "aligned", 0x11);
    HubStateTestAccess::on_schema_registered(s, rec_a);
    HubStateTestAccess::on_schema_registered(s, rec_b);

    auto out = HubStateTestAccess::validate_schema_citation(
        s, "cons.viewer.uid00000005", prod_a, prod_b, "frame",
        rec_b.hash, "aligned");
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

    std::array<uint8_t, 32> wrong_hash;
    wrong_hash.fill(0x99);
    auto out_h = HubStateTestAccess::validate_schema_citation(
        s, "cons.viewer.uid00000005", prod_uid, prod_uid, "frame",
        wrong_hash, "aligned");
    EXPECT_FALSE(out_h.ok());
    EXPECT_EQ(out_h.reason, CitationOutcome::Reason::kFingerprintMismatch);

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
    const std::string ghost = "prod.ghost.uid00000099";
    std::array<uint8_t, 32> any_hash{};
    auto out = HubStateTestAccess::validate_schema_citation(
        s, "cons.viewer.uid00000005", ghost, ghost, "frame",
        any_hash, "aligned");
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.reason, CitationOutcome::Reason::kUnknownOwner);
    EXPECT_EQ(s.counters().schema_citation_rejected_total, 1u);
}

// ─── Wave M2.5 — controlled-access API on ChannelEntry ─────────────────────
//
// These tests pin the contract laid out in
// docs/tech_draft/controlled_access_api_design.md §5.1 + §6.  They are
// the FIRST tests in this codebase to exercise multi-producer
// admission — the pre-M2 scalar producer fields made it structurally
// impossible to write such a test, so the entire overwrite-class bug
// family was hidden by absence.  Until step 3 migrates the broker
// REG_REQ handler to use these methods, the tests drive the API
// directly on a `ChannelEntry` instance — no HubState plumbing
// involved, so the tests are pure API contracts.

namespace
{
using pylabhub::hub::AddConsumerResult;
using pylabhub::hub::AddProducerResult;
using pylabhub::hub::RemoveProducerResult;

ProducerEntry make_producer(const std::string &uid, uint64_t pid)
{
    ProducerEntry p;
    p.role_uid       = uid;
    p.role_name      = uid + "-name";
    p.producer_pid   = pid;
    return p;
}
} // namespace

TEST(ChannelEntryApi, AddProducer_FirstSucceeds)
{
    ChannelEntry ch;
    ch.name = "ch.camera.test";
    ch.data_transport = "zmq";

    auto r = ch.add_producer(make_producer("prod.camA.uid00000001", 1001));
    EXPECT_EQ(r, AddProducerResult::Created);
    EXPECT_EQ(ch.producer_count(), 1u);
    ASSERT_NE(ch.find_producer("prod.camA.uid00000001"), nullptr);
}

TEST(ChannelEntryApi, AddProducer_SameUidRejected_NoStateMutation)
{
    // §6.2 contract: same-uid is rejected with RejectedUidConflict
    // regardless of whether the existing entry is active or
    // stale-residue.  The reject path must NOT mutate any state on
    // ChannelEntry (no overwrite, no append, no inbox/metadata
    // change).
    ChannelEntry ch;
    ch.name = "ch.camera.test";
    ch.data_transport = "zmq";

    ProducerEntry first  = make_producer("prod.camA.uid00000001", 1001);
    first.inbox_endpoint  = "tcp://10.0.0.1:9001";
    first.zmq_node_endpoint = "tcp://10.0.0.1:5001";
    ASSERT_EQ(ch.add_producer(first), AddProducerResult::Created);

    ProducerEntry collision = make_producer("prod.camA.uid00000001", 9999);
    collision.inbox_endpoint    = "tcp://10.0.0.2:9999";  // different — must NOT overwrite
    collision.zmq_node_endpoint = "tcp://10.0.0.2:5999";  // different — must NOT overwrite

    auto r = ch.add_producer(std::move(collision));
    EXPECT_EQ(r, AddProducerResult::RejectedUidConflict);

    // State unchanged after reject:
    ASSERT_EQ(ch.producer_count(), 1u);
    const auto *p = ch.find_producer("prod.camA.uid00000001");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->producer_pid,       1001u);
    EXPECT_EQ(p->inbox_endpoint,     "tcp://10.0.0.1:9001");
    EXPECT_EQ(p->zmq_node_endpoint,  "tcp://10.0.0.1:5001");
}

TEST(ChannelEntryApi, AddProducer_TwoDistinctUidsOnZmqChannel_BothLive_IndependentFields)
{
    // The CONTRACT LOCK: a Fan-In channel admits two distinct
    // producers; each keeps its own data-plane endpoint, inbox
    // metadata, and metadata blob.  Pre-M2.5 the scalar
    // ChannelEntry fields silently overwrote on the second
    // registration; this test fails on that legacy behaviour and
    // passes on the new API.
    ChannelEntry ch;
    ch.name = "ch.camera.fanin";
    ch.data_transport = "zmq";

    ProducerEntry a = make_producer("prod.camA.uid00000001", 1001);
    a.zmq_node_endpoint = "tcp://10.0.0.1:5001";
    a.inbox_endpoint    = "tcp://10.0.0.1:9001";
    a.metadata          = nlohmann::json::object({{"camera", "A"}, {"serial", "SN-001"}});

    ProducerEntry b = make_producer("prod.camB.uid00000002", 1002);
    b.zmq_node_endpoint = "tcp://10.0.0.2:5002";
    b.inbox_endpoint    = "tcp://10.0.0.2:9002";
    b.metadata          = nlohmann::json::object({{"camera", "B"}, {"serial", "SN-002"}});

    ASSERT_EQ(ch.add_producer(std::move(a)), AddProducerResult::Created);
    ASSERT_EQ(ch.add_producer(std::move(b)), AddProducerResult::Created);
    ASSERT_EQ(ch.producer_count(), 2u);

    const auto *pa = ch.find_producer("prod.camA.uid00000001");
    const auto *pb = ch.find_producer("prod.camB.uid00000002");
    ASSERT_NE(pa, nullptr);
    ASSERT_NE(pb, nullptr);

    EXPECT_EQ(pa->zmq_node_endpoint, "tcp://10.0.0.1:5001");
    EXPECT_EQ(pb->zmq_node_endpoint, "tcp://10.0.0.2:5002");
    EXPECT_EQ(pa->inbox_endpoint,    "tcp://10.0.0.1:9001");
    EXPECT_EQ(pb->inbox_endpoint,    "tcp://10.0.0.2:9002");
    EXPECT_EQ(pa->metadata.at("serial").get<std::string>(), "SN-001");
    EXPECT_EQ(pb->metadata.at("serial").get<std::string>(), "SN-002");
}

TEST(ChannelEntryApi, AddProducer_ShmCardinality_SecondRejected)
{
    // §6.3 contract: SHM channels admit exactly one producer; the
    // second add_producer must reject with RejectedShmCardinality,
    // even when the uid is distinct (the SHM physical constraint is
    // separate from the same-uid rule).
    ChannelEntry ch;
    ch.name = "ch.thermo.shm";
    ch.data_transport = "shm";

    ASSERT_EQ(ch.add_producer(make_producer("prod.thermoA.uid00000003", 3001)),
              AddProducerResult::Created);

    auto r = ch.add_producer(make_producer("prod.thermoB.uid00000004", 3002));
    EXPECT_EQ(r, AddProducerResult::RejectedShmCardinality);
    EXPECT_EQ(ch.producer_count(), 1u);
    EXPECT_NE(ch.find_producer("prod.thermoA.uid00000003"), nullptr);
    EXPECT_EQ(ch.find_producer("prod.thermoB.uid00000004"), nullptr);
}

TEST(ChannelEntryApi, AddConsumer_SameUidRejected_NoStateMutation)
{
    ChannelEntry ch;
    ch.name = "ch.viewer.test";
    ch.data_transport = "zmq";

    ConsumerEntry first = make_consumer("cons.view1.uid00000010");
    first.inbox_endpoint = "tcp://10.0.1.1:8001";
    ASSERT_EQ(ch.add_consumer(std::move(first)), AddConsumerResult::Created);

    ConsumerEntry collision = make_consumer("cons.view1.uid00000010");
    collision.inbox_endpoint = "tcp://10.0.1.2:8002";  // must NOT overwrite

    auto r = ch.add_consumer(std::move(collision));
    EXPECT_EQ(r, AddConsumerResult::RejectedUidConflict);
    ASSERT_EQ(ch.consumer_count(), 1u);
    EXPECT_EQ(ch.find_consumer("cons.view1.uid00000010")->inbox_endpoint,
              "tcp://10.0.1.1:8001");
}

TEST(ChannelEntryApi, RemoveProducer_TracksChannelEmpty)
{
    // Atomic teardown (HEP-CORE-0023 §2.1.1) hinges on remove_producer
    // returning channel_now_empty so the caller can drop the channel
    // only when the last producer leaves.
    ChannelEntry ch;
    ch.name = "ch.camera.fanin";
    ch.data_transport = "zmq";
    ASSERT_EQ(ch.add_producer(make_producer("prod.camA.uid00000001", 1001)),
              AddProducerResult::Created);
    ASSERT_EQ(ch.add_producer(make_producer("prod.camB.uid00000002", 1002)),
              AddProducerResult::Created);

    auto r = ch.remove_producer("prod.camA.uid00000001");
    EXPECT_TRUE(r.removed);
    EXPECT_FALSE(r.channel_now_empty);
    EXPECT_EQ(ch.producer_count(), 1u);

    auto r2 = ch.remove_producer("prod.camB.uid00000002");
    EXPECT_TRUE(r2.removed);
    EXPECT_TRUE(r2.channel_now_empty);
    EXPECT_EQ(ch.producer_count(), 0u);

    // Removing a non-present uid is a no-op; channel_now_empty
    // reflects the current state (empty), but `removed` is false.
    auto r3 = ch.remove_producer("prod.ghost.uid00000099");
    EXPECT_FALSE(r3.removed);
    EXPECT_TRUE(r3.channel_now_empty);
}

TEST(ChannelEntryApi, AggregateMetadataTree_KeyedByProducerUid)
{
    // §6.1 wire-shape contract: aggregate_metadata_tree() returns
    // a JSON object keyed by producer role_uid.  Producers with
    // null metadata are omitted (NOT present-as-null), so consumers
    // can rely on the result being an object whose keys map to
    // non-null blobs.
    ChannelEntry ch;
    ch.name = "ch.fanin.meta";
    ch.data_transport = "zmq";

    ProducerEntry a = make_producer("prod.alpha.uid00000020", 5001);
    a.metadata = nlohmann::json::object({{"role", "alpha"}, {"build", "2026-05-10"}});
    ProducerEntry b = make_producer("prod.beta.uid00000021",  5002);
    b.metadata = nlohmann::json::object({{"role", "beta"}});
    ProducerEntry c = make_producer("prod.gamma.uid00000022", 5003);
    // c.metadata remains null — must be omitted from the tree.

    ASSERT_EQ(ch.add_producer(std::move(a)), AddProducerResult::Created);
    ASSERT_EQ(ch.add_producer(std::move(b)), AddProducerResult::Created);
    ASSERT_EQ(ch.add_producer(std::move(c)), AddProducerResult::Created);

    nlohmann::json tree = ch.aggregate_metadata_tree();
    ASSERT_TRUE(tree.is_object());
    EXPECT_EQ(tree.size(), 2u);
    EXPECT_TRUE(tree.contains("prod.alpha.uid00000020"));
    EXPECT_TRUE(tree.contains("prod.beta.uid00000021"));
    EXPECT_FALSE(tree.contains("prod.gamma.uid00000022"));
    EXPECT_EQ(tree["prod.alpha.uid00000020"]["build"].get<std::string>(),
              "2026-05-10");
}

TEST(ChannelEntryApi, AggregateMetadataTree_Empty_ReturnsEmptyObject)
{
    // The contract: result is `{}`, never `null`, so consumers can
    // rely on the field being an object.
    ChannelEntry ch;
    ch.name = "ch.empty.meta";
    nlohmann::json tree = ch.aggregate_metadata_tree();
    EXPECT_TRUE(tree.is_object());
    EXPECT_EQ(tree.size(), 0u);
}

TEST(ChannelEntryApi, SetProducerInbox_KeyedByUid)
{
    // Inbox endpoint update on A must NOT mutate B's inbox endpoint
    // (HEP-CORE-0027 §3 per-producer inbox).
    ChannelEntry ch;
    ch.name = "ch.fanin.inbox";
    ch.data_transport = "zmq";
    ASSERT_EQ(ch.add_producer(make_producer("prod.camA.uid00000001", 1001)),
              AddProducerResult::Created);
    ASSERT_EQ(ch.add_producer(make_producer("prod.camB.uid00000002", 1002)),
              AddProducerResult::Created);

    EXPECT_TRUE(ch.set_producer_inbox(
        "prod.camA.uid00000001",
        "tcp://10.0.0.1:7001", R"([{"name":"k","type":"int32"}])",
        "aligned", "enforced"));

    const auto *pa = ch.find_producer("prod.camA.uid00000001");
    const auto *pb = ch.find_producer("prod.camB.uid00000002");
    EXPECT_EQ(pa->inbox_endpoint,    "tcp://10.0.0.1:7001");
    EXPECT_EQ(pa->inbox_packing,     "aligned");
    EXPECT_EQ(pa->inbox_checksum,    "enforced");
    // B remains untouched.
    EXPECT_EQ(pb->inbox_endpoint,    "");
    EXPECT_EQ(pb->inbox_packing,     "");

    // Missing uid is a no-op (returns false).
    EXPECT_FALSE(ch.set_producer_inbox("prod.ghost.uid00000099",
                                         "tcp://10.0.0.99:7099", "[]",
                                         "aligned", "none"));
}

TEST(ChannelEntryApi, SetConsumerInbox_KeyedByUid)
{
    ChannelEntry ch;
    ch.name = "ch.viewer.inbox";
    ch.data_transport = "zmq";

    ConsumerEntry a = make_consumer("cons.view1.uid00000010");
    ConsumerEntry b = make_consumer("cons.view2.uid00000011");
    ASSERT_EQ(ch.add_consumer(std::move(a)), AddConsumerResult::Created);
    ASSERT_EQ(ch.add_consumer(std::move(b)), AddConsumerResult::Created);

    EXPECT_TRUE(ch.set_consumer_inbox(
        "cons.view1.uid00000010",
        "tcp://10.0.1.1:8001", R"([])", "packed", "none"));
    EXPECT_EQ(ch.find_consumer("cons.view1.uid00000010")->inbox_endpoint,
              "tcp://10.0.1.1:8001");
    EXPECT_EQ(ch.find_consumer("cons.view1.uid00000010")->inbox_packing,
              "packed");
    // Sibling untouched.
    EXPECT_EQ(ch.find_consumer("cons.view2.uid00000011")->inbox_endpoint, "");

    EXPECT_FALSE(ch.set_consumer_inbox("cons.ghost.uid00000099",
                                         "tcp://10.0.1.99:8099", "[]",
                                         "aligned", "none"));
}

TEST(ChannelEntryApi, SetProducerMetadata_KeyedByUid_AndProducerMetadataLookup)
{
    // set_producer_metadata writes to ProducerEntry.metadata, keyed by
    // uid.  producer_metadata(uid) returns nullptr for missing uids and
    // a non-null pointer otherwise; aggregate_metadata_tree() is
    // tested separately.  Pin both the set + the lookup contract.
    ChannelEntry ch;
    ch.name = "ch.fanin.meta-set";
    ch.data_transport = "zmq";
    ASSERT_EQ(ch.add_producer(make_producer("prod.alphaA.uid00000020", 5001)),
              AddProducerResult::Created);
    ASSERT_EQ(ch.add_producer(make_producer("prod.betaB.uid00000021",  5002)),
              AddProducerResult::Created);

    EXPECT_TRUE(ch.set_producer_metadata("prod.alphaA.uid00000020",
        nlohmann::json::object({{"k","v-a"}})));
    EXPECT_TRUE(ch.set_producer_metadata("prod.betaB.uid00000021",
        nlohmann::json::object({{"k","v-b"}})));

    const auto *ma = ch.producer_metadata("prod.alphaA.uid00000020");
    const auto *mb = ch.producer_metadata("prod.betaB.uid00000021");
    ASSERT_NE(ma, nullptr);
    ASSERT_NE(mb, nullptr);
    EXPECT_EQ(ma->at("k").get<std::string>(), "v-a");
    EXPECT_EQ(mb->at("k").get<std::string>(), "v-b");

    // Missing uid → nullptr; set returns false.
    EXPECT_EQ(ch.producer_metadata("prod.ghost.uid00000099"), nullptr);
    EXPECT_FALSE(ch.set_producer_metadata("prod.ghost.uid00000099",
                                            nlohmann::json::object()));
}

TEST(ChannelEntryApi, ProducerZmqNodeEndpoint_LookupAccessor)
{
    // producer_zmq_node_endpoint(uid) is the getter counterpart of
    // set_producer_zmq_node_endpoint — returns nullopt for missing
    // uid, the stored value otherwise.
    ChannelEntry ch;
    ch.name = "ch.fanin.endpoint-get";
    ch.data_transport = "zmq";

    ProducerEntry a = make_producer("prod.alphaA.uid00000020", 5001);
    a.zmq_node_endpoint = "tcp://10.0.0.1:5101";
    ASSERT_EQ(ch.add_producer(std::move(a)), AddProducerResult::Created);

    auto ep = ch.producer_zmq_node_endpoint("prod.alphaA.uid00000020");
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(*ep, "tcp://10.0.0.1:5101");

    EXPECT_FALSE(ch.producer_zmq_node_endpoint("prod.ghost.uid00000099").has_value());
}

TEST(ChannelEntryApi, SetProducerZmqPubkey_PerProducerStorage)
{
    // HEP-CORE-0021 §5.2: per-producer CURVE pubkey.  Wave M2.5
    // step 2c adds the field on ProducerEntry; step 3 will migrate
    // the REG_REQ handler to populate it.  This test pins the API
    // contract: set/get is per-producer, sibling untouched.
    ChannelEntry ch;
    ch.name = "ch.fanin.pubkey";
    ch.data_transport = "zmq";
    ASSERT_EQ(ch.add_producer(make_producer("prod.alphaA.uid00000020", 5001)),
              AddProducerResult::Created);
    ASSERT_EQ(ch.add_producer(make_producer("prod.betaB.uid00000021", 5002)),
              AddProducerResult::Created);

    EXPECT_TRUE(ch.set_producer_zmq_pubkey("prod.alphaA.uid00000020",
        "ALPHA-CURVE-KEY-Z85"));
    auto pka = ch.producer_zmq_pubkey("prod.alphaA.uid00000020");
    auto pkb = ch.producer_zmq_pubkey("prod.betaB.uid00000021");
    ASSERT_TRUE(pka.has_value());
    ASSERT_TRUE(pkb.has_value());
    EXPECT_EQ(*pka, "ALPHA-CURVE-KEY-Z85");
    EXPECT_EQ(*pkb, "");  // B untouched

    EXPECT_FALSE(ch.set_producer_zmq_pubkey("prod.ghost.uid00000099", "X"));
    EXPECT_FALSE(ch.producer_zmq_pubkey("prod.ghost.uid00000099").has_value());
}

TEST(ChannelEntryApi, RemoveConsumer_PresentAndMissing)
{
    ChannelEntry ch;
    ch.name = "ch.viewer.test";
    ch.data_transport = "zmq";
    ASSERT_EQ(ch.add_consumer(make_consumer("cons.view1.uid00000010")),
              AddConsumerResult::Created);
    ASSERT_EQ(ch.add_consumer(make_consumer("cons.view2.uid00000011")),
              AddConsumerResult::Created);
    ASSERT_EQ(ch.consumer_count(), 2u);

    EXPECT_TRUE(ch.remove_consumer("cons.view1.uid00000010"));
    EXPECT_EQ(ch.consumer_count(), 1u);
    EXPECT_EQ(ch.find_consumer("cons.view1.uid00000010"), nullptr);
    EXPECT_NE(ch.find_consumer("cons.view2.uid00000011"), nullptr);

    // Missing → returns false; state unchanged.
    EXPECT_FALSE(ch.remove_consumer("cons.ghost.uid00000099"));
    EXPECT_EQ(ch.consumer_count(), 1u);
}

TEST(ChannelEntryApi, IsShmAndCounts_DerivedFromState)
{
    ChannelEntry ch;
    ch.name = "ch.thermo.shm-flag";
    ch.data_transport = "shm";
    EXPECT_TRUE(ch.is_shm());
    EXPECT_EQ(ch.producer_count(), 0u);
    EXPECT_EQ(ch.consumer_count(), 0u);

    ASSERT_EQ(ch.add_producer(make_producer("prod.thermoA.uid00000003", 3001)),
              AddProducerResult::Created);
    ASSERT_EQ(ch.add_consumer(make_consumer("cons.viewer.uid00000010")),
              AddConsumerResult::Created);
    ASSERT_EQ(ch.add_consumer(make_consumer("cons.viewer2.uid00000011")),
              AddConsumerResult::Created);
    EXPECT_EQ(ch.producer_count(), 1u);
    EXPECT_EQ(ch.consumer_count(), 2u);

    ChannelEntry zmq_ch;
    zmq_ch.data_transport = "zmq";
    EXPECT_FALSE(zmq_ch.is_shm());
}

TEST(ChannelEntryApi, SetProducerZmqNodeEndpoint_KeyedByUid)
{
    // ENDPOINT_UPDATE_REQ scoping: a producer's endpoint update must
    // not mutate any other producer's endpoint on the same channel.
    ChannelEntry ch;
    ch.name = "ch.camera.fanin";
    ch.data_transport = "zmq";

    ProducerEntry a = make_producer("prod.camA.uid00000001", 1001);
    a.zmq_node_endpoint = "tcp://10.0.0.1:0";  // unresolved port
    ProducerEntry b = make_producer("prod.camB.uid00000002", 1002);
    b.zmq_node_endpoint = "tcp://10.0.0.2:5002";

    ASSERT_EQ(ch.add_producer(std::move(a)), AddProducerResult::Created);
    ASSERT_EQ(ch.add_producer(std::move(b)), AddProducerResult::Created);

    EXPECT_TRUE(ch.set_producer_zmq_node_endpoint(
        "prod.camA.uid00000001", "tcp://10.0.0.1:5111"));

    EXPECT_EQ(ch.find_producer("prod.camA.uid00000001")->zmq_node_endpoint,
              "tcp://10.0.0.1:5111");
    // B's endpoint must NOT have been disturbed:
    EXPECT_EQ(ch.find_producer("prod.camB.uid00000002")->zmq_node_endpoint,
              "tcp://10.0.0.2:5002");

    // Updating a non-present uid is a no-op (returns false).
    EXPECT_FALSE(ch.set_producer_zmq_node_endpoint(
        "prod.ghost.uid00000099", "tcp://10.0.0.99:5099"));
}

// ─── Wave M2.5 step 3 — _on_producer_added admission op ─────────────
//
// These tests pin the controlled-access REG_REQ admission contract
// per docs/tech_draft/controlled_access_api_design.md §7.5.5.  Each
// scenario verifies: (1) typed ProducerAdmissionResult, (2) channel
// state after the call (no partial mutation on reject), (3) handler
// fanout (ch_opened fires only on first producer; role_reg fires on
// every Created).

namespace
{
using pylabhub::hub::ChannelSchemaInvariants;
using pylabhub::hub::ChannelTransportInvariants;
using pylabhub::hub::InvariantSetResult;
using pylabhub::hub::ProducerAdmissionResult;

ChannelSchemaInvariants make_schema_invariants(const std::string &hash =
                                                  std::string(64, 'a'))
{
    ChannelSchemaInvariants s;
    s.schema_hash    = hash;
    s.schema_version = 1;
    return s;
}

ChannelTransportInvariants make_zmq_transport()
{
    ChannelTransportInvariants t;
    t.has_shared_memory = false;
    t.shm_name          = "";
    t.pattern           = pylabhub::hub::ChannelPattern::PubSub;
    t.data_transport    = "zmq";
    return t;
}

ChannelTransportInvariants make_shm_transport(const std::string &shm_name)
{
    ChannelTransportInvariants t;
    t.has_shared_memory = true;
    t.shm_name          = shm_name;
    t.pattern           = pylabhub::hub::ChannelPattern::PubSub;
    t.data_transport    = "shm";
    return t;
}
} // namespace

TEST(HubStateProducerAdmission, FirstProducer_FreshChannel_OpensAndFires)
{
    HubState s;
    std::vector<std::string> opened, role_reg;
    s.subscribe_channel_opened(
        [&](const ChannelEntry &e) { opened.push_back(e.name); });
    s.subscribe_role_registered(
        [&](const RoleEntry &r) { role_reg.push_back(r.uid); });

    auto r = HubStateTestAccess::on_producer_added(
        s, "ch.fanin.first",
        make_schema_invariants(),
        make_zmq_transport(),
        make_producer("prod.camA.uid00000001", 1001));

    EXPECT_EQ(r.producer_result,  AddProducerResult::Created);
    EXPECT_EQ(r.invariant_result, InvariantSetResult::Created);
    EXPECT_TRUE(r.channel_opened);
    EXPECT_TRUE(r.mismatched_invariant.empty());

    EXPECT_EQ(opened, (std::vector<std::string>{"ch.fanin.first"}));
    EXPECT_EQ(role_reg, (std::vector<std::string>{"prod.camA.uid00000001"}));

    auto ch = s.channel("ch.fanin.first");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 1u);
    EXPECT_EQ(ch->find_producer("prod.camA.uid00000001")->producer_pid, 1001u);
    EXPECT_EQ(ch->schema_hash, std::string(64, 'a'));
}

TEST(HubStateProducerAdmission, SecondDistinctUid_MatchingInvariants_Appends)
{
    HubState s;
    int opened_count = 0;
    std::vector<std::string> role_reg;
    s.subscribe_channel_opened([&](const ChannelEntry &) { ++opened_count; });
    s.subscribe_role_registered(
        [&](const RoleEntry &r) { role_reg.push_back(r.uid); });

    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.append",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);

    auto r = HubStateTestAccess::on_producer_added(
        s, "ch.fanin.append",
        make_schema_invariants(),        // same hash
        make_zmq_transport(),
        make_producer("prod.camB.uid00000002", 1002));

    EXPECT_EQ(r.producer_result,  AddProducerResult::Created);
    EXPECT_EQ(r.invariant_result, InvariantSetResult::IdempotentEqual);
    EXPECT_FALSE(r.channel_opened);
    EXPECT_TRUE(r.mismatched_invariant.empty());

    // ch_opened fired exactly once (only on the first admission).
    EXPECT_EQ(opened_count, 1);
    // role_reg fired twice (once per producer).
    EXPECT_EQ(role_reg, (std::vector<std::string>{
                            "prod.camA.uid00000001",
                            "prod.camB.uid00000002"}));

    auto ch = s.channel("ch.fanin.append");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 2u);
}

TEST(HubStateProducerAdmission, SecondProducer_SchemaMismatch_Rejected_NoStateMutation)
{
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.schema-mismatch",
                  make_schema_invariants(std::string(64, 'a')),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);

    auto schema_b = make_schema_invariants(std::string(64, 'b'));
    auto r = HubStateTestAccess::on_producer_added(
        s, "ch.fanin.schema-mismatch",
        schema_b,
        make_zmq_transport(),
        make_producer("prod.camB.uid00000002", 1002));

    EXPECT_EQ(r.invariant_result, InvariantSetResult::RejectedMismatch);
    EXPECT_EQ(r.mismatched_invariant, "schema_hash");

    // No state mutation — producer count unchanged, B's uid absent.
    auto ch = s.channel("ch.fanin.schema-mismatch");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 1u);
    EXPECT_EQ(ch->find_producer("prod.camB.uid00000002"), nullptr);
}

TEST(HubStateProducerAdmission, SameUidRedo_Rejected_UidConflict)
{
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.uid-redo",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);

    auto r = HubStateTestAccess::on_producer_added(
        s, "ch.fanin.uid-redo",
        make_schema_invariants(),
        make_zmq_transport(),
        make_producer("prod.camA.uid00000001", 9999));  // same uid

    EXPECT_EQ(r.invariant_result, InvariantSetResult::IdempotentEqual);
    EXPECT_EQ(r.producer_result,  AddProducerResult::RejectedUidConflict);

    // Channel state unchanged — original pid 1001 preserved.
    auto ch = s.channel("ch.fanin.uid-redo");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 1u);
    EXPECT_EQ(ch->find_producer("prod.camA.uid00000001")->producer_pid, 1001u);
}

// ─── Wave M2.5 step 4 — _on_producer_dropped admission op ──────────
//
// Pin the contract that DEREG of one producer on a multi-producer
// channel leaves the channel alive iff other producers remain.  Only
// the LAST producer's leave triggers atomic teardown (HEP-CORE-0023
// §2.1.1).  See controlled_access_api_design.md §7 step 4.

TEST(HubStateProducerDrop, NotFound_ChannelMissing_NoStateMutation)
{
    HubState s;
    auto r = HubStateTestAccess::on_producer_dropped(
        s, "ch.nonexistent.test", "prod.ghost.uid00000001",
        ChannelCloseReason::VoluntaryDereg);
    EXPECT_FALSE(r.removed);
    EXPECT_FALSE(r.channel_now_empty);
}

TEST(HubStateProducerDrop, NotFound_UidMissingOnChannel_NoStateMutation)
{
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.drop-missing",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);

    auto r = HubStateTestAccess::on_producer_dropped(
        s, "ch.fanin.drop-missing", "prod.ghost.uid00000099",
        ChannelCloseReason::VoluntaryDereg);
    EXPECT_FALSE(r.removed);
    EXPECT_FALSE(r.channel_now_empty);

    // Channel and the original producer still present.
    auto ch = s.channel("ch.fanin.drop-missing");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 1u);
    EXPECT_NE(ch->find_producer("prod.camA.uid00000001"), nullptr);
}

TEST(HubStateProducerDrop, NonLastProducer_DropLeavesChannelAlive_NoCloseFired)
{
    // Two producers on a Fan-In channel; drop A.  Channel must
    // survive (channel_now_empty=false) AND the ch_closed handler
    // must NOT fire (atomic teardown is reserved for last-producer-
    // leave per HEP-CORE-0023 §2.1.1).
    HubState s;
    int closed_count = 0;
    s.subscribe_channel_closed([&](const std::string&) { ++closed_count; });

    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.drop-keep",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.drop-keep",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camB.uid00000002", 1002))
                  .producer_result,
              AddProducerResult::Created);
    ASSERT_EQ(closed_count, 0);

    auto r = HubStateTestAccess::on_producer_dropped(
        s, "ch.fanin.drop-keep", "prod.camA.uid00000001",
        ChannelCloseReason::VoluntaryDereg);
    EXPECT_TRUE(r.removed);
    EXPECT_FALSE(r.channel_now_empty);
    EXPECT_EQ(closed_count, 0) << "ch_closed must NOT fire when producers remain";

    auto ch = s.channel("ch.fanin.drop-keep");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 1u);
    EXPECT_EQ(ch->find_producer("prod.camA.uid00000001"), nullptr);
    EXPECT_NE(ch->find_producer("prod.camB.uid00000002"), nullptr);
}

TEST(HubStateProducerDrop, LastProducer_DropTearsChannelDown_FiresClose)
{
    // Atomic teardown contract: the LAST producer's drop fires the
    // close cascade — channel record is gone after the call.
    HubState s;
    std::vector<std::string> closed;
    s.subscribe_channel_closed(
        [&](const std::string &name) { closed.push_back(name); });

    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.drop-last",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);

    auto r = HubStateTestAccess::on_producer_dropped(
        s, "ch.fanin.drop-last", "prod.camA.uid00000001",
        ChannelCloseReason::VoluntaryDereg);
    EXPECT_TRUE(r.removed);
    EXPECT_TRUE(r.channel_now_empty);

    // ch_closed fires; channel record is gone.
    EXPECT_EQ(closed, (std::vector<std::string>{"ch.fanin.drop-last"}));
    EXPECT_FALSE(s.channel("ch.fanin.drop-last").has_value());
}

TEST(HubStateProducerDrop, ChannelClose_FiresOncePerCloseEvent)
{
    // Two-producer setup, drop A then B.  ch_closed must fire EXACTLY
    // ONCE (on the second drop — the last producer's leave).  No
    // partial fire on the first drop, no double-fire after the close.
    HubState s;
    std::vector<std::string> closed;
    s.subscribe_channel_closed(
        [&](const std::string &name) { closed.push_back(name); });

    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.drop-sequence",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.drop-sequence",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camB.uid00000002", 1002))
                  .producer_result,
              AddProducerResult::Created);

    auto r1 = HubStateTestAccess::on_producer_dropped(
        s, "ch.fanin.drop-sequence", "prod.camA.uid00000001",
        ChannelCloseReason::VoluntaryDereg);
    EXPECT_TRUE(r1.removed);
    EXPECT_FALSE(r1.channel_now_empty);
    EXPECT_EQ(closed.size(), 0u);

    auto r2 = HubStateTestAccess::on_producer_dropped(
        s, "ch.fanin.drop-sequence", "prod.camB.uid00000002",
        ChannelCloseReason::VoluntaryDereg);
    EXPECT_TRUE(r2.removed);
    EXPECT_TRUE(r2.channel_now_empty);
    EXPECT_EQ(closed, (std::vector<std::string>{"ch.fanin.drop-sequence"}));
}

// ─── Wave M2.5 step 5 — _set_producer_zmq_node_endpoint HubState op ──────
//
// Pin the contract that ENDPOINT_UPDATE_REQ at the HubState layer
// mutates ONLY the targeted producer's endpoint; sibling producers
// on the same Fan-In channel are unaffected (HEP-CORE-0021 §16.3).

TEST(HubStateProducerEndpointUpdate, KnownChannelKnownUid_UpdatesOnlyTarget)
{
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.ep-update",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.ep-update",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camB.uid00000002", 1002))
                  .producer_result,
              AddProducerResult::Created);

    // Both producers start with no endpoint.
    auto ch_pre = s.channel("ch.fanin.ep-update");
    ASSERT_TRUE(ch_pre.has_value());
    EXPECT_EQ(ch_pre->find_producer("prod.camA.uid00000001")->zmq_node_endpoint, "");
    EXPECT_EQ(ch_pre->find_producer("prod.camB.uid00000002")->zmq_node_endpoint, "");

    // Updating A's endpoint must NOT touch B.
    EXPECT_TRUE(HubStateTestAccess::set_producer_zmq_node_endpoint(
        s, "ch.fanin.ep-update", "prod.camA.uid00000001",
        "tcp://10.0.0.1:5111"));

    auto ch_post = s.channel("ch.fanin.ep-update");
    ASSERT_TRUE(ch_post.has_value());
    EXPECT_EQ(ch_post->find_producer("prod.camA.uid00000001")->zmq_node_endpoint,
              "tcp://10.0.0.1:5111");
    EXPECT_EQ(ch_post->find_producer("prod.camB.uid00000002")->zmq_node_endpoint, "");
}

TEST(HubStateProducerEndpointUpdate, UnknownChannel_ReturnsFalse)
{
    HubState s;
    EXPECT_FALSE(HubStateTestAccess::set_producer_zmq_node_endpoint(
        s, "ch.nonexistent.test", "prod.x.uid00000001",
        "tcp://10.0.0.1:5111"));
}

TEST(HubStateProducerEndpointUpdate, KnownChannelUnknownUid_ReturnsFalse_NoSiblingPerturbation)
{
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.ep-update-ghost",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);

    EXPECT_FALSE(HubStateTestAccess::set_producer_zmq_node_endpoint(
        s, "ch.fanin.ep-update-ghost", "prod.ghost.uid00000099",
        "tcp://10.0.0.99:5111"));

    // A's endpoint must remain empty (unset).
    auto ch = s.channel("ch.fanin.ep-update-ghost");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->find_producer("prod.camA.uid00000001")->zmq_node_endpoint, "");
}

// ─── Wave M2.5 step 6 — multi-producer _on_pending_timeout ──────────
//
// Pin the contract that on a Fan-In channel, one producer's Pending
// timeout drops just that producer; the channel stays alive (HEP-
// CORE-0023 §2.1.1).  The previous channel-level _on_pending_timeout
// would tear the whole channel down on any producer's timeout.

TEST(HubStateProducerPendingTimeout, NonLastProducer_DropsOnlyOne_ChannelSurvives)
{
    HubState s;
    int closed_count = 0;
    s.subscribe_channel_closed([&](const std::string&) { ++closed_count; });

    // Register two producers via the controlled API.
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.pending-survive",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.pending-survive",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camB.uid00000002", 1002))
                  .producer_result,
              AddProducerResult::Created);

    // First heartbeat for both, then force A → Pending via timeout.
    HubStateTestAccess::on_heartbeat(
        s, "ch.fanin.pending-survive", "prod.camA.uid00000001", "producer",
        std::chrono::steady_clock::now(), std::nullopt);
    HubStateTestAccess::on_heartbeat(
        s, "ch.fanin.pending-survive", "prod.camB.uid00000002", "producer",
        std::chrono::steady_clock::now(), std::nullopt);
    HubStateTestAccess::on_heartbeat_timeout(
        s, "ch.fanin.pending-survive", "prod.camA.uid00000001");

    auto pt = HubStateTestAccess::on_pending_timeout(
        s, "ch.fanin.pending-survive", "prod.camA.uid00000001");
    EXPECT_TRUE(pt.removed);
    EXPECT_FALSE(pt.channel_now_empty);
    EXPECT_EQ(closed_count, 0) << "Channel must survive while B remains alive";

    // Channel still exists; A is gone, B remains.
    auto ch = s.channel("ch.fanin.pending-survive");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 1u);
    EXPECT_EQ(ch->find_producer("prod.camA.uid00000001"), nullptr);
    EXPECT_NE(ch->find_producer("prod.camB.uid00000002"), nullptr);
    EXPECT_EQ(s.counters().pending_to_deregistered_total, 1u);
}

TEST(HubStateProducerPendingTimeout, LastProducer_TearsChannelDown)
{
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin.pending-last",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.camA.uid00000001", 1001))
                  .producer_result,
              AddProducerResult::Created);

    HubStateTestAccess::on_heartbeat(
        s, "ch.fanin.pending-last", "prod.camA.uid00000001", "producer",
        std::chrono::steady_clock::now(), std::nullopt);
    HubStateTestAccess::on_heartbeat_timeout(
        s, "ch.fanin.pending-last", "prod.camA.uid00000001");

    auto pt = HubStateTestAccess::on_pending_timeout(
        s, "ch.fanin.pending-last", "prod.camA.uid00000001");
    EXPECT_TRUE(pt.removed);
    EXPECT_TRUE(pt.channel_now_empty);
    EXPECT_FALSE(s.channel("ch.fanin.pending-last").has_value());
    EXPECT_EQ(s.counters().pending_to_deregistered_total, 1u);
}

TEST(HubStateProducerAdmission, SecondProducer_ShmChannel_RejectedShmCardinality)
{
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.thermo.shm-cardinality",
                  make_schema_invariants(),
                  make_shm_transport("ch.thermo.shm-cardinality-shm"),
                  make_producer("prod.thermoA.uid00000003", 3001))
                  .producer_result,
              AddProducerResult::Created);

    auto r = HubStateTestAccess::on_producer_added(
        s, "ch.thermo.shm-cardinality",
        make_schema_invariants(),
        make_shm_transport("ch.thermo.shm-cardinality-shm"),
        make_producer("prod.thermoB.uid00000004", 3002));  // distinct uid

    EXPECT_EQ(r.invariant_result, InvariantSetResult::IdempotentEqual);
    EXPECT_EQ(r.producer_result,  AddProducerResult::RejectedShmCardinality);

    auto ch = s.channel("ch.thermo.shm-cardinality");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 1u);
    EXPECT_EQ(ch->find_producer("prod.thermoB.uid00000004"), nullptr);
}

// ─── Wave M3 — RoleEntry controlled-access API ─────────────────────────────
//
// Pin the contract for RoleEntry::add_presence / on_heartbeat /
// on_heartbeat_timeout / on_pending_timeout / on_dereg + terminal
// cleanup of _set_role_disconnected.  Mirrors the M2.5 contract-lock
// pattern at role/presence scope.  See
// docs/tech_draft/M3_role_entry_controlled_access.md.

namespace
{
using pylabhub::hub::AddPresenceResult;
using pylabhub::hub::HeartbeatEffect;
using pylabhub::hub::TransitionEffect;
} // namespace

TEST(RoleEntryApi, AddPresence_Created)
{
    RoleEntry r = make_role("prod.cam.uid00000001");
    RolePresence *out = nullptr;
    auto eff = r.add_presence("ch.test", "producer", &out);
    EXPECT_EQ(eff, AddPresenceResult::Created);
    ASSERT_EQ(r.presences.size(), 1u);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->channel, "ch.test");
    EXPECT_EQ(out->role_type, "producer");
    EXPECT_EQ(out->state, RoleState::Connected);
    EXPECT_FALSE(out->first_heartbeat_seen);
}

TEST(RoleEntryApi, AddPresence_DuplicateRejected_NoStateMutation)
{
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.test", "producer"), AddPresenceResult::Created);

    // Same (channel, role_type) re-add → RejectedDuplicate; presences
    // count unchanged.
    auto eff = r.add_presence("ch.test", "producer");
    EXPECT_EQ(eff, AddPresenceResult::RejectedDuplicate);
    EXPECT_EQ(r.presences.size(), 1u);

    // Different (channel, role_type) tuple → Created (multi-presence).
    EXPECT_EQ(r.add_presence("ch.test", "consumer"), AddPresenceResult::Created);
    EXPECT_EQ(r.presences.size(), 2u);

    // Yet another tuple — different channel — also OK (cross-channel role).
    EXPECT_EQ(r.add_presence("ch.other", "producer"), AddPresenceResult::Created);
    EXPECT_EQ(r.presences.size(), 3u);
}

TEST(RoleEntryApi, RemovePresence_PresentAndMissing)
{
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.test", "producer"), AddPresenceResult::Created);
    ASSERT_EQ(r.add_presence("ch.test", "consumer"), AddPresenceResult::Created);

    EXPECT_TRUE(r.remove_presence("ch.test", "producer"));
    EXPECT_EQ(r.presences.size(), 1u);
    EXPECT_EQ(r.find_presence("ch.test", "producer"), nullptr);
    EXPECT_NE(r.find_presence("ch.test", "consumer"), nullptr);

    EXPECT_FALSE(r.remove_presence("ch.test", "producer"));  // already gone
    EXPECT_FALSE(r.remove_presence("ch.ghost", "producer"));
    EXPECT_EQ(r.presences.size(), 1u);
}

TEST(RoleEntryApi, OnHeartbeat_FirstHeartbeat_NewlyConnected)
{
    // Newly-added presence has first_heartbeat_seen=false; first
    // heartbeat flips it true.  HeartbeatEffect captures this via
    // was_first_heartbeat_seen=false → coarse enum NewlyConnected.
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.test", "producer"), AddPresenceResult::Created);

    auto eff = r.on_heartbeat("ch.test", "producer",
                              std::chrono::steady_clock::now());
    EXPECT_TRUE(eff.presence_found);
    EXPECT_EQ(eff.prev_state, RoleState::Connected);
    EXPECT_FALSE(eff.was_first_heartbeat_seen);
    EXPECT_EQ(eff.to_transition_effect(), TransitionEffect::NewlyConnected);

    // Post-condition: state Connected + first_heartbeat_seen true.
    const auto *p = r.find_presence("ch.test", "producer");
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->first_heartbeat_seen);
    EXPECT_EQ(p->state, RoleState::Connected);
}

TEST(RoleEntryApi, OnHeartbeat_SteadyState_Refreshed)
{
    // Second heartbeat on already-Connected + first_heartbeat_seen presence
    // → Refreshed (just an update; no transition).
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.test", "producer"), AddPresenceResult::Created);
    r.on_heartbeat("ch.test", "producer", std::chrono::steady_clock::now());

    auto eff = r.on_heartbeat("ch.test", "producer",
                              std::chrono::steady_clock::now());
    EXPECT_TRUE(eff.presence_found);
    EXPECT_EQ(eff.prev_state, RoleState::Connected);
    EXPECT_TRUE(eff.was_first_heartbeat_seen);
    EXPECT_EQ(eff.to_transition_effect(), TransitionEffect::Refreshed);
}

TEST(RoleEntryApi, OnHeartbeat_PendingRecovery_NewlyConnected_PrevPending)
{
    // Pending → Connected via heartbeat.  prev_state == Pending is the
    // signal callers (HubState::_on_heartbeat) use to bump
    // pending_to_ready_total.
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.test", "producer"), AddPresenceResult::Created);
    r.on_heartbeat("ch.test", "producer", std::chrono::steady_clock::now());
    ASSERT_EQ(r.on_heartbeat_timeout("ch.test", "producer"),
              TransitionEffect::ToPending);

    auto eff = r.on_heartbeat("ch.test", "producer",
                              std::chrono::steady_clock::now());
    EXPECT_TRUE(eff.presence_found);
    EXPECT_EQ(eff.prev_state, RoleState::Pending);
    EXPECT_TRUE(eff.was_first_heartbeat_seen);
    EXPECT_EQ(eff.to_transition_effect(), TransitionEffect::NewlyConnected);
}

TEST(RoleEntryApi, OnHeartbeat_PresenceNotFound_NoChange)
{
    RoleEntry r = make_role("prod.cam.uid00000001");
    auto eff = r.on_heartbeat("ch.ghost", "producer",
                              std::chrono::steady_clock::now());
    EXPECT_FALSE(eff.presence_found);
    EXPECT_EQ(eff.to_transition_effect(), TransitionEffect::NoChange);
}

TEST(RoleEntryApi, OnHeartbeatTimeout_ConnectedToPending_OnlyConnected)
{
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.test", "producer"), AddPresenceResult::Created);

    // Connected → Pending.
    EXPECT_EQ(r.on_heartbeat_timeout("ch.test", "producer"),
              TransitionEffect::ToPending);
    EXPECT_EQ(r.find_presence("ch.test", "producer")->state, RoleState::Pending);

    // Already Pending → NoChange (handles lost-race case).
    EXPECT_EQ(r.on_heartbeat_timeout("ch.test", "producer"),
              TransitionEffect::NoChange);

    // After Disconnect, also NoChange.
    EXPECT_EQ(r.on_pending_timeout("ch.test", "producer"),
              TransitionEffect::ToDisconnected);
    EXPECT_EQ(r.on_heartbeat_timeout("ch.test", "producer"),
              TransitionEffect::NoChange);

    // Missing presence → NoChange.
    EXPECT_EQ(r.on_heartbeat_timeout("ch.ghost", "producer"),
              TransitionEffect::NoChange);
}

TEST(RoleEntryApi, OnPendingTimeout_PendingToDisconnected_OnlyPending)
{
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.test", "producer"), AddPresenceResult::Created);

    // Connected (not Pending) → NoChange.
    EXPECT_EQ(r.on_pending_timeout("ch.test", "producer"),
              TransitionEffect::NoChange);

    // Connected → Pending → Disconnected.
    ASSERT_EQ(r.on_heartbeat_timeout("ch.test", "producer"),
              TransitionEffect::ToPending);
    EXPECT_EQ(r.on_pending_timeout("ch.test", "producer"),
              TransitionEffect::ToDisconnected);
    EXPECT_EQ(r.find_presence("ch.test", "producer")->state,
              RoleState::Disconnected);

    // Already Disconnected → NoChange.
    EXPECT_EQ(r.on_pending_timeout("ch.test", "producer"),
              TransitionEffect::NoChange);
}

TEST(RoleEntryApi, OnDereg_AnyStateToDisconnected_NoOpIfAlreadyDisconnected)
{
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.a", "producer"), AddPresenceResult::Created);
    ASSERT_EQ(r.add_presence("ch.b", "producer"), AddPresenceResult::Created);
    r.on_heartbeat("ch.a", "producer", std::chrono::steady_clock::now());

    // Connected → Disconnected.
    EXPECT_EQ(r.on_dereg("ch.a", "producer"),
              TransitionEffect::ToDisconnected);
    EXPECT_EQ(r.find_presence("ch.a", "producer")->state,
              RoleState::Disconnected);

    // Already-Disconnected → NoChange.
    EXPECT_EQ(r.on_dereg("ch.a", "producer"),
              TransitionEffect::NoChange);

    // Sibling (ch.b) untouched.
    EXPECT_EQ(r.find_presence("ch.b", "producer")->state,
              RoleState::Connected);
}

// ─── Terminal cleanup contract (M3 step 4) ─────────────────────────────────

TEST(RoleEntryApi, SetRoleDisconnected_TerminalErase_IdempotentSecondCall)
{
    // The contract that retires the disconnected_fired PATCH:
    // _set_role_disconnected fires the handler exactly once AND
    // erases the RoleEntry.  Second call finds no entry and is a
    // no-op (idempotent BY CONSTRUCTION — terminal-erase IS the
    // memoization).
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.term"));
    ASSERT_TRUE(s.role("prod.main.test").has_value());

    int fired = 0;
    s.subscribe_role_disconnected([&](const std::string &) { ++fired; });

    HubStateTestAccess::set_role_disconnected(s, "prod.main.test");
    EXPECT_EQ(fired, 1);
    EXPECT_FALSE(s.role("prod.main.test").has_value())
        << "Role entry must be erased after disconnect (M3 step 4 contract)";

    HubStateTestAccess::set_role_disconnected(s, "prod.main.test");
    EXPECT_EQ(fired, 1) << "Second call is idempotent — no entry to re-fire from";
}

TEST(RoleEntryApi, SetRoleDisconnected_SchemaCascadeFires_HubGlobalsImmune)
{
    // The schema-cascade contract was already tested in
    // HubStateSchemas.RoleDisconnect_CascadeEvictsOwnedSchemas (which
    // continues to pass post-M3 step 4).  This test re-pins the
    // contract under the terminal-erase semantics: cascade fires
    // BEFORE the entry is erased, so SchemaRecords keyed by the
    // owner uid are evicted; hub-globals (owner=="hub") are immune.
    HubState s;
    const std::string uid = "prod.cam.uid01234567";
    HubStateTestAccess::set_role_registered(s, make_role(uid));
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec(uid, "frame", "aligned", 0xAA));
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("hub", "lab.demo.frame@1"));
    ASSERT_EQ(s.schema_count(), 2u);

    HubStateTestAccess::set_role_disconnected(s, uid);

    EXPECT_FALSE(s.role(uid).has_value())
        << "Role entry erased (M3 terminal cleanup)";
    EXPECT_EQ(s.schema_count(), 1u) << "Hub-global survives";
    EXPECT_TRUE(s.schema("hub", "lab.demo.frame@1").has_value());
    EXPECT_FALSE(s.schema(uid, "frame").has_value())
        << "Owner-uid schema evicted by cascade";
}
