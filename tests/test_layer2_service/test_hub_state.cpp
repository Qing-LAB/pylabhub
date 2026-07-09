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
#include "utils/logger.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "hub_state_test_access.h"
#include "binary_lifecycle.h"
#include "curve_test_setup.h"

#include <gtest/gtest.h>

// Parent-lifecycle bringup — SMS is a member of the mod pack per the
// SMS five-point pattern (HEP-CORE-0043 §2.2).  Tests below rely on
// `secure().keys()` and the `secure().keys()` shim being valid.
PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule(),
    pylabhub::utils::security::SecureSubsystem::GetLifecycleModule())

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

using pylabhub::hub::BandEntry;
using pylabhub::hub::BandMember;
using pylabhub::hub::ChannelCloseReason;
using pylabhub::hub::ChannelEntry;
using pylabhub::hub::ChannelObservable;
using pylabhub::hub::ChannelTopology;
using pylabhub::hub::parse_channel_topology;
using pylabhub::hub::to_string;
using pylabhub::hub::transport_topology_compatible;
using pylabhub::hub::check_topology_against_stored;
using pylabhub::hub::check_cardinality_admission;
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
    r.short_tag = tag;
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
        [&](const std::string &, const std::string &uid,
            const std::string & /*reason*/) { left.push_back(uid); });

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
    EXPECT_EQ(r->short_tag,   "prod");
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
    // HEP-CORE-0036 §5b.4: SHM segment name is the channel name (no
    // separate `shm_name` field — it was always == channel_name).
    EXPECT_EQ(shm->block_path, "ch1");

    EXPECT_EQ(s.counters().msg_type_counts.count("REG_REQ"), 0u);
    EXPECT_EQ(opened_fired, (std::vector<std::string>{"ch1"}));
    EXPECT_EQ(role_fired, (std::vector<std::string>{"prod.main.test"}));
}

TEST(HubStateOps, ChannelRegistered_NoShm_SkipsShmBlock)
{
    HubState s;
    auto     ch         = make_channel("ch1");
    // HEP-CORE-0036 §5b.4: data_transport == "shm" classifies SHM;
    // anything else means no SHM block registered.
    ch.data_transport   = "zmq";
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

TEST(HubStateOps, ChannelClosed_RemovesChannelAndErasesRoleEntry)
{
    // Wave M3 step 5b contract (2026-05-11): when channel close is
    // the role's ONLY presence transitioning Disconnected,
    // `_on_channel_closed` calls `_dispatch_role_disconnected_if_dead`
    // which erases the role entry (HEP-CORE-0023 §2.6 + HEP-CORE-0034
    // §7.2 schema cascade).  This replaces the pre-M3 "scrub" pattern
    // that left a stale RoleEntry with a Disconnected presence row.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    ASSERT_EQ(s.role("prod.main.test")->channels.size(), 1u);

    HubStateTestAccess::on_channel_closed(
        s, "ch1", ChannelCloseReason::VoluntaryDereg);
    EXPECT_FALSE(s.channel("ch1").has_value());
    EXPECT_FALSE(s.shm_block("ch1").has_value());
    // Terminal cleanup — entry gone, no stale Disconnected residue.
    EXPECT_FALSE(s.role("prod.main.test").has_value())
        << "Wave M3 step 5b — single-presence channel close must erase "
           "the role entry; old pre-M3 behavior left it as residue.";

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
    // Wave M3 step 5h (2026-05-11): on_dereg erases the presence row
    // rather than leaving a Disconnected tombstone.  ch1 producer-
    // presence is GONE; ch2 producer-presence stays Connected.
    EXPECT_EQ(r->find_presence("ch1", "producer"), nullptr)
        << "Tombstone removal — Disconnected presence rows are erased";
    const auto *p2 = r->find_presence("ch2", "producer");
    ASSERT_NE(p2, nullptr);
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
    EXPECT_EQ(r->short_tag, "cons");
    EXPECT_EQ(r->name,     "A");
    EXPECT_EQ(r->channels.size(), 1u);
    EXPECT_EQ(r->channels[0], "ch1");
    // Consumer-presence created at REG time per HEP-CORE-0023 §2.6.
    ASSERT_NE(r->find_presence("ch1", "consumer"), nullptr);
}

TEST(HubStateOps, ConsumerLeft_RemovesFromChannelAndErasesRoleEntry)
{
    // Wave M3 step 5b contract (2026-05-11): when the consumer's
    // only presence is its (ch, "consumer") row, `_on_consumer_left`
    // routes through `on_dereg` (Disconnected) then dispatches
    // `_set_role_disconnected`, which erases the entry.  Pre-M3
    // behavior left the role with a stale Disconnected presence row.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch1"));
    HubStateTestAccess::on_consumer_joined(s, "ch1", make_consumer("cons.A.test"));
    HubStateTestAccess::on_consumer_left(s, "ch1", "cons.A.test");

    auto ch = s.channel("ch1");
    ASSERT_TRUE(ch.has_value());
    EXPECT_TRUE(ch->consumers.empty());

    // Producer role (`prod.main.test`) is still alive (its presence on
    // ch1 stays Connected); consumer role's only presence is now gone
    // → entry erased.
    EXPECT_FALSE(s.role("cons.A.test").has_value())
        << "Wave M3 step 5b — single-presence consumer DEREG must "
           "erase the role entry.";
    EXPECT_TRUE(s.role("prod.main.test").has_value())
        << "Producer role on the same channel must be unaffected.";
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
    EXPECT_EQ(r->short_tag, "prod");
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

// Wave M1.4 channel_metrics_snapshot tests deferred to after the
// `make_producer` / `make_schema_invariants` / `make_zmq_transport`
// helper definitions (they live in a later anon namespace).  Search
// for "HubStateChannelMetricsSnapshot" below.

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

    // CURVE is unconditional (HEP-CORE-0035 §2); HEP-CORE-0040 §172
    // moved hub identity into the process KeyStore, so BrokerService
    // ctor now requires the "hub_identity" entry to be present.
    // Construct a seed_curve_identities for an ephemeral hub identity
    // — this test never calls run() so no socket is bound, but the
    // ctor's KeyStore check still fires.
    auto setup = pylabhub::tests::make_curve_setup({});
    pylabhub::tests::seed_curve_identities(setup);

    pylabhub::broker::BrokerService::Config cfg;
    cfg.endpoint = "tcp://127.0.0.1:0";
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

// #161 Phase 1 mutation pin: HEP-CORE-0040 §172 specifies the
// BrokerService ctor MUST resolve the hub identity from the KeyStore
// under the literal name "hub_identity".  This negative test seeds
// the KeyStore with a different name only and asserts that the ctor
// throws std::logic_error with a message naming "hub_identity".
//
// Mutations this pins:
//   - Changing the literal "hub_identity" to anything else (e.g.
//     "hub_id", "hub.identity") in the ctor check.
//   - Removing the throw (e.g. degrading to `return false` or
//     silent fallthrough).
//   - Changing the thrown exception type from std::logic_error to
//     std::runtime_error or similar.
//   - Inverting the condition (using `if (has(...))` to throw,
//     which would break the positive plumbing test above).
TEST(BrokerServiceCtor, MissingHubIdentityInKeyStoreThrowsLogicError)
{
    namespace sec = pylabhub::utils::security;


    // Seed an UNRELATED name so the KeyStore is non-empty but the
    // ctor's specific lookup for "hub_identity" misses.  Catching
    // "empty KeyStore" as the failure mode would let a future
    // refactor that uses some other name (e.g. "broker_identity")
    // silently pass — this assertion forces the ctor to use the
    // exact HEP-CORE-0040 §172 contract name.
    const auto kp = pylabhub::tests::gen_curve_keypair();
    sec::secure().keys().add_identity_from_z85(
        "not_hub_identity", kp.public_z85, kp.secret_z85);

    pylabhub::hub::HubState state;
    pylabhub::broker::BrokerService::Config cfg;
    cfg.endpoint = "tcp://127.0.0.1:0";

    try
    {
        pylabhub::broker::BrokerService broker(cfg, state);
        ADD_FAILURE() << "Expected BrokerService ctor to throw "
                         "std::logic_error when 'hub_identity' is "
                         "absent from KeyStore; ctor returned normally.";
    }
    catch (const std::logic_error &e)
    {
        const std::string what = e.what();
        EXPECT_NE(what.find("hub_identity"), std::string::npos)
            << "Ctor threw, but the message must name the missing "
               "literal 'hub_identity' so an operator can correct the "
               "vault / fixture wiring.  Got: " << what;
    }
    catch (const std::exception &e)
    {
        ADD_FAILURE() << "Ctor threw, but the type must be "
                         "std::logic_error (programmer-error contract, "
                         "HEP-CORE-0035 §4.6.5 no-bypass).  Got: "
                      << typeid(e).name() << " — " << e.what();
    }
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

// HEP-CORE-0036 §5b.4: ChannelTransportInvariants holds only the
// canonical `data_transport` classifier; pre-§5b duplicates retired.
ChannelTransportInvariants make_zmq_transport()
{
    ChannelTransportInvariants t;
    t.data_transport    = "zmq";
    return t;
}

ChannelTransportInvariants make_shm_transport(const std::string & /*shm_name*/)
{
    ChannelTransportInvariants t;
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

// ──────────────────────────────────────────────────────────────────────
// Wave M3 step 5c-e (2026-05-11) — cache invariant + schema cascade
// owner-lifetime tests.  Pin the H9 / H10 / H11 / H12 / H13 contracts
// surfaced by the second-pass review.
// ──────────────────────────────────────────────────────────────────────

TEST(HubStateProducerDropped,
     MultiProducer_VoluntaryDereg_TransitionsPresenceAndCleansCache)
{
    // H9 contract: non-last producer voluntary DEREG on a Fan-In
    // channel.  The role's `(channel, "producer")` presence must
    // transition Disconnected (via `on_dereg`) AND the `channels` cache
    // must be cleaned (via `drop_channel_if_orphaned`).  Without this,
    // the role shows a ghost-Connected producer-presence on a channel
    // where it no longer holds a slot.
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.h9.fanin",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.fanA.uid00000001", 1001))
                  .producer_result, AddProducerResult::Created);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.h9.fanin",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.fanB.uid00000002", 1002))
                  .producer_result, AddProducerResult::Created);

    auto r = HubStateTestAccess::on_producer_dropped(
        s, "ch.h9.fanin", "prod.fanA.uid00000001",
        ChannelCloseReason::VoluntaryDereg);
    EXPECT_TRUE(r.removed);
    EXPECT_FALSE(r.channel_now_empty);

    // Channel still alive with just B.
    auto ch = s.channel("ch.h9.fanin");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 1u);
    EXPECT_NE(ch->find_producer("prod.fanB.uid00000002"), nullptr);
    EXPECT_EQ(ch->find_producer("prod.fanA.uid00000001"), nullptr);

    // Role A erased via terminal cleanup (its only presence was the
    // producer on this channel; presence Disconnected → dispatch →
    // erase).  Without H9 fix, A's presence would stay Connected and
    // the role entry would linger.
    EXPECT_FALSE(s.role("prod.fanA.uid00000001").has_value())
        << "H9 + H1 dispatch: role with no alive presences must be erased "
           "after voluntary DEREG on a Fan-In channel";

    // Role B unaffected.
    EXPECT_TRUE(s.role("prod.fanB.uid00000002").has_value());
}

TEST(HubStateProducerDropped,
     MultiChannel_Producer_StaysAliveAfterOneDereg_SchemasSurvive)
{
    // H12 contract: schema eviction is owner-lifetime ONLY
    // (HEP-CORE-0034 §7.2).  Before the H12 fix, the per-producer
    // cascade fired from `_on_channel_closed` would evict owner-
    // namespaced schemas the producer is still using on OTHER
    // channels.  After H12: schemas survive until the OWNER role
    // fully disconnects.
    HubState s;

    // Producer X on two distinct channels (single-producer each).
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.h12.a",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.multich.uid00000001", 1001))
                  .producer_result, AddProducerResult::Created);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.h12.b",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.multich.uid00000001", 1001))
                  .producer_result, AddProducerResult::Created);

    // X registers two owner-namespaced schemas.
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("prod.multich.uid00000001", "frame", "aligned", 0xAA));
    HubStateTestAccess::on_schema_registered(
        s, make_schema_rec("prod.multich.uid00000001", "inbox", "aligned", 0xBB));
    ASSERT_EQ(s.schema_count(), 2u);

    // X DEREGs from ch.h12.a (its only producer → channel teardown).
    auto r = HubStateTestAccess::on_producer_dropped(
        s, "ch.h12.a", "prod.multich.uid00000001",
        ChannelCloseReason::VoluntaryDereg);
    EXPECT_TRUE(r.removed);
    EXPECT_TRUE(r.channel_now_empty);
    EXPECT_FALSE(s.channel("ch.h12.a").has_value());

    // ch.h12.b still alive; X still alive there.
    EXPECT_TRUE(s.channel("ch.h12.b").has_value());
    auto role = s.role("prod.multich.uid00000001");
    ASSERT_TRUE(role.has_value())
        << "Role must survive while it has presence on ch.h12.b";

    // Cache invariant: ch.h12.a dropped from cache; ch.h12.b remains.
    EXPECT_EQ(role->channels.size(), 1u);
    EXPECT_EQ(role->channels[0], "ch.h12.b");

    // Wave M3 step 5h: presence on ch.h12.a is ERASED (tombstone
    // removal), on ch.h12.b remains Connected.
    EXPECT_EQ(role->find_presence("ch.h12.a", "producer"), nullptr);
    const auto *p_b = role->find_presence("ch.h12.b", "producer");
    ASSERT_NE(p_b, nullptr);
    EXPECT_EQ(p_b->state, RoleState::Connected);

    // Critical H12 assertion: schemas MUST survive.  The pre-H12
    // per-producer cascade would have evicted both records here even
    // though X is still alive on ch.h12.b.
    EXPECT_EQ(s.schema_count(), 2u)
        << "H12: producer schemas must survive while role is alive on "
           "other channels";
    EXPECT_TRUE(s.schema("prod.multich.uid00000001", "frame").has_value());
    EXPECT_TRUE(s.schema("prod.multich.uid00000001", "inbox").has_value());
    EXPECT_EQ(s.counters().schema_evicted_total, 0u)
        << "Owner still alive → no eviction yet (HEP-CORE-0034 §7.2)";

    // Now close ch.h12.b too → role fully disconnects → schemas evict.
    HubStateTestAccess::on_channel_closed(
        s, "ch.h12.b", ChannelCloseReason::AdminClose);
    EXPECT_FALSE(s.role("prod.multich.uid00000001").has_value())
        << "Role erased after last presence Disconnected";
    EXPECT_EQ(s.schema_count(), 0u)
        << "H12: schemas evict via dispatch (owner-lifetime) once role dies";
    EXPECT_EQ(s.counters().schema_evicted_total, 2u);
}

TEST(HubStateChannelClosed, ConsumerPresence_AtomicallyTransitionsDisconnected)
{
    // H13 + HEP-CORE-0023 §2.1.1 atomic teardown: when a channel
    // closes, ALL its presences transition Disconnected at the same
    // moment.  Before this fix, consumer-presences stayed Connected
    // until the eventual CONSUMER_DEREG_REQ — leaving a stale
    // Connected presence on a non-existent channel.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.h13"));
    HubStateTestAccess::on_consumer_joined(s, "ch.h13",
                                            make_consumer("cons.x.test"));

    // Pre-condition: consumer presence Connected.
    {
        auto r = s.role("cons.x.test");
        ASSERT_TRUE(r.has_value());
        const auto *p = r->find_presence("ch.h13", "consumer");
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->state, RoleState::Connected);
    }

    HubStateTestAccess::on_channel_closed(
        s, "ch.h13", ChannelCloseReason::AdminClose);

    // Both roles cleaned up via dispatch — producer (prod.main.test) and
    // consumer (cons.x.test) each had only this channel.  H13:
    // consumer-presence transitioned at atomic teardown, so
    // dispatch sees `any_presence_alive() == false` and erases.
    EXPECT_FALSE(s.role("prod.main.test").has_value())
        << "Producer role erased (only presence Disconnected)";
    EXPECT_FALSE(s.role("cons.x.test").has_value())
        << "Consumer role erased (consumer-presence atomically "
           "Disconnected per HEP-CORE-0023 §2.1.1)";
    EXPECT_FALSE(s.channel("ch.h13").has_value());
}

TEST(HubStateBandCascade, TerminalCleanup_RemovesUidFromAllBands_FiresBandLeftWithReason)
{
    // Wave M3 step 5f+i (2026-05-11) — H16/H20 contract.  When a role
    // terminal-cleans (e.g., via _dispatch_role_disconnected_if_dead),
    // the cascade walks pImpl->bands, removes the uid from every band's
    // members, auto-deletes empty bands, and fires `band_left` with
    // reason="role_closed".  Replaces the previous broker-imperative
    // path that fired too eagerly (eviction at channel-close even when
    // the role survived on other channels).
    HubState s;
    HubStateTestAccess::set_role_registered(s, make_role("prod.cam.uid01234567"));
    BandMember m;
    m.role_uid = "prod.cam.uid01234567";
    m.role_name = "cam";
    m.zmq_identity = "id-cam";
    HubStateTestAccess::set_band_joined(s, "!alpha", m);
    HubStateTestAccess::set_band_joined(s, "!beta",  m);

    std::vector<std::tuple<std::string,std::string,std::string>> band_left_events;
    s.subscribe_band_left(
        [&](const std::string &band, const std::string &uid,
            const std::string &reason) {
            band_left_events.emplace_back(band, uid, reason);
        });

    ASSERT_TRUE(s.band("!alpha").has_value());
    ASSERT_TRUE(s.band("!beta").has_value());

    // Force terminal cleanup (admin-style force-erase) — exercises the
    // same cascade as the production dispatch path.
    HubStateTestAccess::set_role_disconnected(s, "prod.cam.uid01234567");

    EXPECT_FALSE(s.role("prod.cam.uid01234567").has_value())
        << "Role erased by terminal cleanup";
    EXPECT_FALSE(s.band("!alpha").has_value())
        << "Empty band auto-deleted after last member left";
    EXPECT_FALSE(s.band("!beta").has_value())
        << "Empty band auto-deleted after last member left";

    ASSERT_EQ(band_left_events.size(), 2u);
    // Order can be either since unordered_map iteration order is unspecified.
    std::sort(band_left_events.begin(), band_left_events.end());
    EXPECT_EQ(std::get<0>(band_left_events[0]), "!alpha");
    EXPECT_EQ(std::get<0>(band_left_events[1]), "!beta");
    for (const auto &[band, uid, reason] : band_left_events)
    {
        EXPECT_EQ(uid,    "prod.cam.uid01234567");
        EXPECT_EQ(reason, "role_closed");
    }
}

TEST(HubStateBandCascade, MultiPresenceRole_StaysAlive_KeepsBandMembership)
{
    // Wave M3 step 5f semantic correction: pre-fix, the broker fired
    // imperative band_on_role_closed from every channel-close fanout.
    // For a multi-presence role X (producer on A, consumer on B), when
    // channel A closes (last producer A → atomic teardown), X was
    // incorrectly evicted from bands even though X is still alive on
    // channel B.  Post-fix: band membership tracks role-lifetime, not
    // channel-lifetime; X keeps band membership until its LAST presence
    // dies.
    HubState s;
    // Register producer X on channel A.
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.bandcascade.a",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.cam.uid01234567", 1001))
                  .producer_result, AddProducerResult::Created);
    // X also consumes channel B (different role admission path).
    HubStateTestAccess::on_channel_registered(s, [&]{
        ChannelEntry e;
        e.name           = "ch.bandcascade.b";
        e.schema_hash    = std::string(64, 'a');
        e.schema_version = 1;
        ProducerEntry p;
        p.producer_pid = 9999;
        p.role_uid     = "prod.other.uid88888888";
        p.role_name    = "other";
        e.producers.push_back(std::move(p));
        return e;
    }());
    HubStateTestAccess::on_consumer_joined(s, "ch.bandcascade.b",
                                            make_consumer("prod.cam.uid01234567"));

    // X joins band !shared.
    BandMember m;
    m.role_uid = "prod.cam.uid01234567";
    m.role_name = "cam";
    m.zmq_identity = "id-cam";
    HubStateTestAccess::set_band_joined(s, "!shared", m);

    // Close channel A (last producer X → atomic teardown).
    HubStateTestAccess::on_channel_closed(
        s, "ch.bandcascade.a", ChannelCloseReason::AdminClose);

    // X's producer-presence on A is Disconnected; X's consumer-presence
    // on B is still Connected.  Role X is alive.  Band membership must
    // be intact.
    auto r = s.role("prod.cam.uid01234567");
    ASSERT_TRUE(r.has_value())
        << "X is alive on channel B (consumer)";
    auto b = s.band("!shared");
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(b->members.size(), 1u);
    EXPECT_EQ(b->members[0].role_uid, "prod.cam.uid01234567")
        << "X keeps band membership while alive on B "
           "(H16 semantic fix: band tracks role-lifetime, not channel-lifetime)";
}

TEST(HubStateCacheInvariant,
     RoleWithBothProducerAndConsumer_SameChannel_PartialDeregKeepsChannel)
{
    // H10 cache invariant: `channels` contains `c` iff at least one
    // alive presence references `c`.  If a role is both producer AND
    // consumer on the same channel, deregistering ONE type must NOT
    // drop the channel from the cache while the other is still alive.
    HubState s;

    // Producer + consumer with the SAME role_uid on the same channel.
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.h10",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.dual.uid00000001", 1001))
                  .producer_result, AddProducerResult::Created);
    HubStateTestAccess::on_consumer_joined(
        s, "ch.h10", make_consumer("prod.dual.uid00000001"));

    {
        auto r = s.role("prod.dual.uid00000001");
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->channels.size(), 1u);
        EXPECT_EQ(r->presences.size(), 2u)
            << "Both producer + consumer presences on the same channel";
    }

    // Consumer DEREGs.  Cache must NOT drop ch.h10 because producer-
    // presence still references it.
    HubStateTestAccess::on_consumer_left(s, "ch.h10",
                                          "prod.dual.uid00000001");

    auto r = s.role("prod.dual.uid00000001");
    ASSERT_TRUE(r.has_value())
        << "Role survives — producer-presence still Connected";
    EXPECT_EQ(r->channels.size(), 1u)
        << "H10 cache invariant: ch.h10 stays in cache because "
           "producer-presence is still alive";
    EXPECT_EQ(r->channels[0], "ch.h10");

    // Now also DEREG the producer (last producer → channel teardown
    // → atomic transition of all presences).
    auto drop = HubStateTestAccess::on_producer_dropped(
        s, "ch.h10", "prod.dual.uid00000001",
        ChannelCloseReason::VoluntaryDereg);
    EXPECT_TRUE(drop.removed);
    EXPECT_TRUE(drop.channel_now_empty);
    EXPECT_FALSE(s.role("prod.dual.uid00000001").has_value())
        << "Role erased after all presences Disconnected";
}

TEST(HubStateLifecycle, ReRegister_AfterPartialDereg_PresenceFreshConnected)
{
    // Wave M3 step 5h (2026-05-11) — H17 contract.  Pre-fix scenario:
    // role X is producer on channel A AND consumer on channel B; X
    // DEREGs producer on A but role survives (consumer on B still
    // alive); X re-REGs producer on A.  Pre-fix, `upsert_presence_row_locked`
    // found the existing Disconnected presence and was a no-op,
    // leaving the role in a three-view-mismatch state until first
    // heartbeat.  Post-fix (H18 erase-on-dereg subsumes H17): the
    // earlier DEREG erased the row; re-REG creates a FRESH Connected
    // row.  No stale tombstone.
    HubState s;
    // Producer X on channel A.
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.rereg.a",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.dual.uid00000001", 1001))
                  .producer_result, AddProducerResult::Created);
    // X also consumer on channel B (we create B with a different producer
    // so X's role is only consumer there).
    HubStateTestAccess::on_channel_registered(s, [&]{
        ChannelEntry e;
        e.name           = "ch.rereg.b";
        e.schema_hash    = std::string(64, 'a');
        e.schema_version = 1;
        ProducerEntry p;
        p.producer_pid = 9999;
        p.role_uid     = "prod.other.uid88888888";
        p.role_name    = "other";
        e.producers.push_back(std::move(p));
        return e;
    }());
    HubStateTestAccess::on_consumer_joined(s, "ch.rereg.b",
                                            make_consumer("prod.dual.uid00000001"));

    auto pre_dereg = s.role("prod.dual.uid00000001");
    ASSERT_TRUE(pre_dereg.has_value());
    EXPECT_EQ(pre_dereg->presences.size(), 2u);

    // X DEREGs as producer on A (last producer → channel A teardown).
    // Wait — actually X is the only producer on A.  So DEREG triggers
    // channel close → cascade marks consumer-presence on B too?  No —
    // B is a different channel; only A's presences transition.
    HubStateTestAccess::on_channel_closed(
        s, "ch.rereg.a", ChannelCloseReason::AdminClose);

    // Post-DEREG: X's producer-presence on A is erased; consumer-
    // presence on B intact.
    auto post_dereg = s.role("prod.dual.uid00000001");
    ASSERT_TRUE(post_dereg.has_value());
    EXPECT_EQ(post_dereg->presences.size(), 1u)
        << "Producer-presence on A is gone (erased); only consumer-"
           "presence on B remains";
    EXPECT_EQ(post_dereg->find_presence("ch.rereg.a", "producer"), nullptr);
    EXPECT_NE(post_dereg->find_presence("ch.rereg.b", "consumer"), nullptr);

    // X re-REGs as producer on A.  Channel A is gone, so we admit X
    // fresh via on_producer_added (recreating channel A).
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.rereg.a",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.dual.uid00000001", 1001))
                  .producer_result, AddProducerResult::Created);

    auto post_rereg = s.role("prod.dual.uid00000001");
    ASSERT_TRUE(post_rereg.has_value());
    const auto *p = post_rereg->find_presence("ch.rereg.a", "producer");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->state, RoleState::Connected)
        << "H17 contract: re-REG creates a fresh Connected presence "
           "(no stale Disconnected tombstone — H18 fix removed the row)";
    EXPECT_FALSE(p->first_heartbeat_seen)
        << "Fresh presence in 'registering' sub-state";
}

TEST(HubStateLifecycle, ChannelChurn_NoTombstoneAccumulation)
{
    // Wave M3 step 5h (2026-05-11) — H18 contract.  A long-lived role
    // that attaches/detaches many channels must not accumulate
    // tombstones in `presences[]`.  Each DEREG erases the row;
    // `presences.size()` stays bounded by the number of currently-live
    // attachments.
    HubState s;
    constexpr int kCycles = 50;
    for (int i = 0; i < kCycles; ++i)
    {
        const std::string ch = "ch.churn." + std::to_string(i);
        ASSERT_EQ(HubStateTestAccess::on_producer_added(
                      s, ch,
                      make_schema_invariants(),
                      make_zmq_transport(),
                      make_producer("prod.churn.uid00000001", 1001))
                      .producer_result, AddProducerResult::Created);
        HubStateTestAccess::on_channel_closed(
            s, ch, ChannelCloseReason::AdminClose);
        // After each cycle, role is fully disconnected → terminal
        // cleanup erases the role.  Re-REG below creates a fresh role.
        EXPECT_FALSE(s.role("prod.churn.uid00000001").has_value())
            << "Cycle " << i << ": role erased after channel teardown";
    }

    // Final state: no leaks anywhere.
    EXPECT_FALSE(s.role("prod.churn.uid00000001").has_value());
    EXPECT_EQ(s.snapshot().channels.size(), 0u);
    EXPECT_EQ(s.schema_count(), 0u);
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

// ──────────────────────────────────────────────────────────────────────
// Wave M1.4 (2026-05-11) — channel_metrics_snapshot tests.  Replace the
// retired `MetricsReported_StoresOnPresenceWithoutLivenessSideEffect`
// test (the dedicated `_on_metrics_reported` op + `METRICS_REPORT_REQ`
// wire message are gone; metrics piggyback on HEARTBEAT_REQ per
// HEP-CORE-0019 §2.3 Phase 6).  Pin the new contract: per-presence-row
// metrics aggregated by HubState::channel_metrics_snapshot.
// ──────────────────────────────────────────────────────────────────────

TEST(HubStateChannelMetricsSnapshot, UnknownChannel_ReturnsEmpty_PairedWithKnown)
{
    // Test pair: empty result for unknown channel must NOT be the same
    // "always empty" output a broken helper would return.  Set up a
    // known-good channel with metrics, then probe a separate unknown
    // name — known returns non-empty, unknown returns empty.
    // A mutation that always returns {} fails the known-good check.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.known"));
    HubStateTestAccess::on_heartbeat(s, "ch.known", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"qps", 1}});

    // Baseline: known channel returns NON-EMPTY metrics.
    auto m_known = s.channel_metrics_snapshot("ch.known");
    ASSERT_TRUE(m_known.contains("producers"))
        << "Helper must return non-empty for a known channel with "
           "metrics — a 'returns {} unconditionally' mutation would "
           "fail this assertion";
    EXPECT_EQ(m_known["producers"]["prod.main.test"]["qps"], 1);

    // Negative: unknown channel returns empty.
    auto m_unknown = s.channel_metrics_snapshot("no.such.channel");
    EXPECT_TRUE(m_unknown.empty())
        << "Unknown channel returns empty object — and the paired "
           "known-channel assertion above proves this isn't because "
           "the helper is broken.";
}

TEST(HubStateChannelMetricsSnapshot, ChannelExistsNoMetrics_PairedWithMetricsChannel)
{
    // Same defensive pair: one channel HAS metrics (positive baseline),
    // another channel exists but no heartbeats reported (negative).
    // Single test, two channels, single helper — catches both
    // "always-empty" and "always-populated" mutations.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.live"));
    HubStateTestAccess::on_heartbeat(s, "ch.live", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"qps", 7}});

    // Empty channel: registered, never heartbeated.  make_channel
    // uses producer "prod.main.test" for BOTH channels; under H18
    // each presence row is per-(channel, role_type), so the empty
    // channel has its own pristine producer presence row with null
    // latest_metrics — separate from the live one.
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.silent"));

    auto m_live   = s.channel_metrics_snapshot("ch.live");
    auto m_silent = s.channel_metrics_snapshot("ch.silent");

    // Baseline: channel with metrics returns non-empty.
    ASSERT_TRUE(m_live.contains("producers"));
    EXPECT_EQ(m_live["producers"]["prod.main.test"]["qps"], 7);

    // Contract: channel registered but no metrics reported → empty
    // object.  The helper walks ChannelEntry.producers/consumers and
    // skips presences with null latest_metrics, so the silent
    // channel's existing producer row contributes nothing.
    EXPECT_TRUE(m_silent.empty())
        << "Registered channel without any heartbeat-with-metrics "
           "returns empty object; the paired ch.live check proves "
           "the helper isn't simply returning {} for all inputs.";
}

TEST(HubStateChannelMetricsSnapshot, SingleProducer_IncludesPidAndMetrics)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.solo"));
    nlohmann::json metrics = {{"qps", 100}, {"errors", 0}};
    HubStateTestAccess::on_heartbeat(s, "ch.solo", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(), metrics);

    auto m = s.channel_metrics_snapshot("ch.solo");
    ASSERT_TRUE(m.contains("producers"));
    ASSERT_TRUE(m["producers"].contains("prod.main.test"));
    EXPECT_EQ(m["producers"]["prod.main.test"]["qps"], 100);
    EXPECT_EQ(m["producers"]["prod.main.test"]["errors"], 0);
    EXPECT_EQ(m["producers"]["prod.main.test"]["pid"], 4242)
        << "pid sourced from ChannelEntry.producers[].producer_pid";
    EXPECT_FALSE(m.contains("consumers"))
        << "No consumers → key omitted";
}

TEST(HubStateChannelMetricsSnapshot, MultiProducer_PerUidNoOverwrite)
{
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.A.uid00000001", 1001))
                  .producer_result, AddProducerResult::Created);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.fanin",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.B.uid00000002", 1002))
                  .producer_result, AddProducerResult::Created);

    // Each producer reports distinct metrics via heartbeat — the H34
    // overwrite-class bug at the metrics layer is eliminated by per-
    // presence-row keying.
    HubStateTestAccess::on_heartbeat(s, "ch.fanin", "prod.A.uid00000001", "producer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"qps", 10}});
    HubStateTestAccess::on_heartbeat(s, "ch.fanin", "prod.B.uid00000002", "producer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"qps", 20}});

    auto m = s.channel_metrics_snapshot("ch.fanin");
    ASSERT_TRUE(m.contains("producers"));
    auto &ps = m["producers"];
    ASSERT_TRUE(ps.contains("prod.A.uid00000001"));
    ASSERT_TRUE(ps.contains("prod.B.uid00000002"));
    EXPECT_EQ(ps["prod.A.uid00000001"]["qps"], 10);
    EXPECT_EQ(ps["prod.B.uid00000002"]["qps"], 20);
    EXPECT_EQ(ps["prod.A.uid00000001"]["pid"], 1001);
    EXPECT_EQ(ps["prod.B.uid00000002"]["pid"], 1002);
}

TEST(HubStateChannelMetricsSnapshot, ProducerAndConsumer_BothPresent)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.mix"));
    HubStateTestAccess::on_consumer_joined(s, "ch.mix",
                                            make_consumer("cons.X.test"));

    HubStateTestAccess::on_heartbeat(s, "ch.mix", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"qps", 50}});
    HubStateTestAccess::on_heartbeat(s, "ch.mix", "cons.X.test", "consumer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"read_count", 99}});

    auto m = s.channel_metrics_snapshot("ch.mix");
    ASSERT_TRUE(m.contains("producers"));
    ASSERT_TRUE(m.contains("consumers"));
    EXPECT_EQ(m["producers"]["prod.main.test"]["qps"], 50);
    EXPECT_EQ(m["consumers"]["cons.X.test"]["read_count"], 99);
    EXPECT_FALSE(m["consumers"]["cons.X.test"].contains("pid"))
        << "Consumers do not get a pid field (pid is per-producer)";
}

TEST(HubStateChannelMetricsSnapshot, Overwrite_LatestWins)
{
    // M1.4 contract: subsequent heartbeats from the same uid REPLACE
    // the prior metrics (HEP-CORE-0019 §2.3 — broker holds only the
    // latest snapshot).  Pre-fix `metrics_store_[ch].producers[uid]`
    // overwrote on every call; post-fix `RolePresence::latest_metrics`
    // overwrites on every `_on_heartbeat` call.  Pin the contract via
    // both the helper AND direct presence inspection.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.over"));

    nlohmann::json m1 = {{"qps", 100}, {"err", 0}};
    HubStateTestAccess::on_heartbeat(s, "ch.over", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(), m1);

    // Helper sees the first reading.
    {
        auto m = s.channel_metrics_snapshot("ch.over");
        EXPECT_EQ(m["producers"]["prod.main.test"]["qps"], 100);
        EXPECT_EQ(m["producers"]["prod.main.test"]["err"], 0);
    }

    nlohmann::json m2 = {{"qps", 200}, {"err", 5}};
    HubStateTestAccess::on_heartbeat(s, "ch.over", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(), m2);

    // Helper now sees the second reading; first is GONE (not merged).
    auto m = s.channel_metrics_snapshot("ch.over");
    EXPECT_EQ(m["producers"]["prod.main.test"]["qps"], 200)
        << "Latest heartbeat must overwrite the prior metrics value";
    EXPECT_EQ(m["producers"]["prod.main.test"]["err"], 5);

    // Direct presence-row inspection — verifies the storage layer
    // itself was overwritten (not just the helper output).
    auto snap = s.snapshot();
    const auto *p = find_producer_in(snap, "ch.over", "prod.main.test");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->latest_metrics, m2)
        << "RolePresence::latest_metrics is the post-overwrite value";
}

TEST(HubStateChannelMetricsSnapshot, NullMetrics_PreservesPriorReading)
{
    // M1.4 contract: a heartbeat that doesn't carry a `metrics` field
    // (std::nullopt) refreshes liveness but DOES NOT clobber
    // `latest_metrics`.  Pre-fix `metrics_store_` had the same
    // behavior (update_*_metrics only called when metrics present);
    // post-fix `_on_heartbeat` guards the write with
    // `if (metrics.has_value()) ...`.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.nullhb"));

    nlohmann::json initial = {{"qps", 42}};
    HubStateTestAccess::on_heartbeat(s, "ch.nullhb", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(), initial);

    // Subsequent heartbeat with NO metrics.
    HubStateTestAccess::on_heartbeat(s, "ch.nullhb", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(), std::nullopt);

    // Prior metrics still visible.
    auto m = s.channel_metrics_snapshot("ch.nullhb");
    ASSERT_TRUE(m.contains("producers"));
    EXPECT_EQ(m["producers"]["prod.main.test"]["qps"], 42)
        << "Null-metrics heartbeat must NOT overwrite prior reading";

    // Direct presence-row inspection: latest_metrics still holds initial.
    auto snap = s.snapshot();
    const auto *p = find_producer_in(snap, "ch.nullhb", "prod.main.test");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->latest_metrics, initial);
}

TEST(HubStateChannelMetricsSnapshot, CrossChannel_Isolated)
{
    // M1.4 contract: metrics live on per-presence rows keyed by
    // (channel, role_type) inside the role's RoleEntry.  Two roles
    // on different channels must not bleed into each other's
    // metrics queries.  The H34 root concern (pre-Phase-6 broker
    // mis-attributed consumer heartbeats to producer-rows) is the
    // overwrite-class bug this test guards against at the storage
    // layer.
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.iso.a",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.A.uid00000001", 1001))
                  .producer_result, AddProducerResult::Created);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.iso.b",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.B.uid00000002", 1002))
                  .producer_result, AddProducerResult::Created);

    HubStateTestAccess::on_heartbeat(s, "ch.iso.a", "prod.A.uid00000001", "producer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"only_on_a", true}});
    HubStateTestAccess::on_heartbeat(s, "ch.iso.b", "prod.B.uid00000002", "producer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"only_on_b", true}});

    auto m_a = s.channel_metrics_snapshot("ch.iso.a");
    auto m_b = s.channel_metrics_snapshot("ch.iso.b");

    // ch.iso.a has prod.A's metrics, NOTHING from prod.B.
    ASSERT_TRUE(m_a.contains("producers"));
    ASSERT_TRUE(m_a["producers"].contains("prod.A.uid00000001"));
    EXPECT_EQ(m_a["producers"]["prod.A.uid00000001"]["only_on_a"], true);
    EXPECT_FALSE(m_a["producers"].contains("prod.B.uid00000002"))
        << "Channel ch.iso.a must NOT see prod.B's metrics";

    // ch.iso.b has prod.B's metrics, NOTHING from prod.A.
    ASSERT_TRUE(m_b.contains("producers"));
    ASSERT_TRUE(m_b["producers"].contains("prod.B.uid00000002"));
    EXPECT_EQ(m_b["producers"]["prod.B.uid00000002"]["only_on_b"], true);
    EXPECT_FALSE(m_b["producers"].contains("prod.A.uid00000001"))
        << "Channel ch.iso.b must NOT see prod.A's metrics";
}

TEST(HubStateChannelMetricsSnapshot, SameUid_ProducerAndConsumer_DistinctRows)
{
    // The H34 ROOT contract: same uid can be a producer AND consumer on
    // the same channel.  Pre-Phase-6, the broker derived role_type from
    // the channel's producer entry and mis-attributed consumer
    // heartbeats to the producer-row.  Post-Phase-6 + M1.4: each
    // heartbeat carries explicit role_type, writes to the matching
    // RolePresence row.  Two distinct presences, two distinct
    // metrics blobs.  Verified via DIRECT inspection of
    // RolePresence::latest_metrics (not just the helper output).
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.dual"));
    // make_channel's producer is "prod.main.test"; consumer joins with
    // the SAME uid so both presences belong to the same role.
    HubStateTestAccess::on_consumer_joined(s, "ch.dual",
                                            make_consumer("prod.main.test"));

    // Pre-heartbeat: NEITHER presence has first_heartbeat_seen set
    // (presence is created at REG-time in "registering" sub-state).
    {
        auto pre  = s.snapshot();
        const auto *p_pre_p = find_presence_in(pre, "ch.dual",
                                                "prod.main.test", "producer");
        const auto *p_pre_c = find_presence_in(pre, "ch.dual",
                                                "prod.main.test", "consumer");
        ASSERT_NE(p_pre_p, nullptr);
        ASSERT_NE(p_pre_c, nullptr);
        ASSERT_FALSE(p_pre_p->first_heartbeat_seen);
        ASSERT_FALSE(p_pre_c->first_heartbeat_seen);
    }

    // Producer heartbeat with role-specific metrics.
    const auto prod_hb_time = std::chrono::steady_clock::now();
    nlohmann::json prod_metrics = {{"qps", 100}, {"kind", "producer-only"}};
    HubStateTestAccess::on_heartbeat(s, "ch.dual", "prod.main.test", "producer",
                                      prod_hb_time, prod_metrics);

    // After producer heartbeat: ONLY producer-presence has
    // first_heartbeat_seen flipped.  This catches the H34-class bug
    // where on_heartbeat matched only on (channel) and updated
    // BOTH presences (or the wrong one) on a single heartbeat.
    {
        auto mid = s.snapshot();
        const auto *p_mid_p = find_presence_in(mid, "ch.dual",
                                                "prod.main.test", "producer");
        const auto *p_mid_c = find_presence_in(mid, "ch.dual",
                                                "prod.main.test", "consumer");
        ASSERT_NE(p_mid_p, nullptr);
        ASSERT_NE(p_mid_c, nullptr);
        EXPECT_TRUE(p_mid_p->first_heartbeat_seen)
            << "Producer-presence saw its first heartbeat (FSM step)";
        EXPECT_FALSE(p_mid_c->first_heartbeat_seen)
            << "Consumer-presence is UNTOUCHED by producer heartbeat — "
               "this is the H34-root contract.  A mutation that drops "
               "role_type from the on_heartbeat lookup would falsely "
               "flip consumer's first_heartbeat_seen here.";
        EXPECT_EQ(p_mid_p->last_heartbeat, prod_hb_time);
    }

    // Consumer heartbeat with DIFFERENT metrics.
    const auto cons_hb_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    nlohmann::json cons_metrics = {{"reads", 50}, {"kind", "consumer-only"}};
    HubStateTestAccess::on_heartbeat(s, "ch.dual", "prod.main.test", "consumer",
                                      cons_hb_time, cons_metrics);

    // DIRECT presence-row inspection — verifies the underlying
    // storage layer, not just the snapshot helper's projection.
    auto snap = s.snapshot();
    const auto *p_prod = find_presence_in(snap, "ch.dual",
                                           "prod.main.test", "producer");
    const auto *p_cons = find_presence_in(snap, "ch.dual",
                                           "prod.main.test", "consumer");
    ASSERT_NE(p_prod, nullptr);
    ASSERT_NE(p_cons, nullptr);
    ASSERT_NE(p_prod, p_cons) << "Producer and consumer must be DISTINCT presence rows";

    // Metrics keyed correctly.
    EXPECT_EQ(p_prod->latest_metrics, prod_metrics)
        << "Producer-presence row carries producer's metrics";
    EXPECT_EQ(p_cons->latest_metrics, cons_metrics)
        << "Consumer-presence row carries consumer's metrics";

    // FSM state keyed correctly: each row's last_heartbeat reflects
    // ONLY its own role's heartbeat (catches H34-root via FSM signal).
    EXPECT_EQ(p_prod->last_heartbeat, prod_hb_time)
        << "Producer last_heartbeat must equal producer-heartbeat time, "
           "NOT consumer-heartbeat time";
    EXPECT_EQ(p_cons->last_heartbeat, cons_hb_time)
        << "Consumer last_heartbeat must equal consumer-heartbeat time, "
           "NOT producer-heartbeat time";
    EXPECT_TRUE(p_prod->first_heartbeat_seen);
    EXPECT_TRUE(p_cons->first_heartbeat_seen);

    // Helper sees both, correctly grouped by role_type.
    auto m = s.channel_metrics_snapshot("ch.dual");
    ASSERT_TRUE(m.contains("producers"));
    ASSERT_TRUE(m.contains("consumers"));
    EXPECT_EQ(m["producers"]["prod.main.test"]["qps"], 100);
    EXPECT_EQ(m["producers"]["prod.main.test"]["kind"], "producer-only");
    EXPECT_EQ(m["consumers"]["prod.main.test"]["reads"], 50);
    EXPECT_EQ(m["consumers"]["prod.main.test"]["kind"], "consumer-only")
        << "Same uid, different role_type → metrics rooted in distinct presence rows";
}

TEST(HubStateChannelMetricsSnapshot, CollectedAt_Progresses_OnEachHeartbeat)
{
    // M1.4 contract: `RolePresence::metrics_collected_at` advances on
    // every heartbeat that carries a metrics payload.  Without this
    // contract, freshness diagnostics break (admins can't tell when
    // the last update happened).
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.time"));

    HubStateTestAccess::on_heartbeat(s, "ch.time", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"v", 1}});
    std::chrono::system_clock::time_point t1;
    {
        auto snap = s.snapshot();
        const auto *p = find_producer_in(snap, "ch.time", "prod.main.test");
        ASSERT_NE(p, nullptr);
        t1 = p->metrics_collected_at;
        EXPECT_NE(t1, std::chrono::system_clock::time_point{})
            << "First heartbeat with metrics stamps a collected_at";
    }

    // Brief wait to ensure timestamps differ.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    HubStateTestAccess::on_heartbeat(s, "ch.time", "prod.main.test", "producer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"v", 2}});
    auto snap = s.snapshot();
    const auto *p = find_producer_in(snap, "ch.time", "prod.main.test");
    ASSERT_NE(p, nullptr);
    EXPECT_GT(p->metrics_collected_at, t1)
        << "Subsequent metrics heartbeat advances metrics_collected_at";
}

TEST(HubStateChannelMetricsSnapshot, RoleDisconnect_MetricsGoAway)
{
    // Wave M3 step 5h: presences erased on disconnect.  Metrics live
    // on the presence row, so they go away naturally — H34 leak class
    // closure (metrics_store_ used to leak per-uid entries after
    // DEREG; per-presence-row keying eliminates that bug class).
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.fade"));
    HubStateTestAccess::on_consumer_joined(s, "ch.fade",
                                            make_consumer("cons.A.test"));
    HubStateTestAccess::on_heartbeat(s, "ch.fade", "cons.A.test", "consumer",
                                      std::chrono::steady_clock::now(),
                                      nlohmann::json{{"read_count", 42}});

    // Pre-DEREG: metrics visible via helper AND directly on the
    // presence row (proves the storage layer holds the data).
    {
        auto m = s.channel_metrics_snapshot("ch.fade");
        ASSERT_TRUE(m.contains("consumers"));
        EXPECT_EQ(m["consumers"]["cons.A.test"]["read_count"], 42);

        auto snap = s.snapshot();
        const auto *p_pre = find_presence_in(snap, "ch.fade",
                                              "cons.A.test", "consumer");
        ASSERT_NE(p_pre, nullptr) << "Consumer presence row exists pre-DEREG";
        EXPECT_EQ(p_pre->latest_metrics["read_count"], 42)
            << "Direct storage inspection: latest_metrics holds the "
               "heartbeat value (not just JSON projection)";
    }

    HubStateTestAccess::on_consumer_left(s, "ch.fade", "cons.A.test");

    // Post-DEREG: consumer presence ERASED (under H18) — both
    // helper output AND direct snapshot must agree.  A mutation
    // that left the presence row as a tombstone (Disconnected
    // state) would be caught by the direct check: find_presence
    // would return a non-null pointer.
    auto m = s.channel_metrics_snapshot("ch.fade");
    EXPECT_FALSE(m.contains("consumers"))
        << "Helper output: consumer metrics absent after DEREG";

    auto snap = s.snapshot();
    EXPECT_EQ(find_presence_in(snap, "ch.fade", "cons.A.test", "consumer"),
              nullptr)
        << "Direct storage inspection: consumer presence row ERASED "
           "(not left as a Disconnected tombstone).  A mutation that "
           "marks Disconnected instead of erasing would fail this.";

    // Role X had only this one presence → entire role entry should
    // also be erased by terminal cleanup (Wave M3 H1+H5).
    EXPECT_FALSE(s.role("cons.A.test").has_value())
        << "Role entry also erased — terminal-cleanup chain ran "
           "through `_dispatch_role_disconnected_if_dead` per "
           "Wave M3 step 5b";
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

    // Wave M3 step 5h: on_pending_timeout ERASES the presence row.
    // Subsequent on_heartbeat_timeout finds no row → NoChange.
    EXPECT_EQ(r.on_pending_timeout("ch.test", "producer"),
              TransitionEffect::ToDisconnected);
    EXPECT_EQ(r.find_presence("ch.test", "producer"), nullptr);
    EXPECT_EQ(r.on_heartbeat_timeout("ch.test", "producer"),
              TransitionEffect::NoChange);

    // Missing presence → NoChange.
    EXPECT_EQ(r.on_heartbeat_timeout("ch.ghost", "producer"),
              TransitionEffect::NoChange);
}

TEST(RoleEntryApi, OnPendingTimeout_PendingToDisconnected_ErasesPresenceRow)
{
    // Wave M3 step 5h (2026-05-11): on_pending_timeout ERASES the
    // presence row rather than marking Disconnected.  Eliminates the
    // tombstone-accumulation bug class for long-lived roles that churn
    // channels.  any_presence_alive() and the channels-cache invariant
    // simplify accordingly.
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.test", "producer"), AddPresenceResult::Created);

    // Connected (not Pending) → NoChange, row stays.
    EXPECT_EQ(r.on_pending_timeout("ch.test", "producer"),
              TransitionEffect::NoChange);
    EXPECT_NE(r.find_presence("ch.test", "producer"), nullptr);

    // Connected → Pending → Disconnected (erase).
    ASSERT_EQ(r.on_heartbeat_timeout("ch.test", "producer"),
              TransitionEffect::ToPending);
    EXPECT_EQ(r.on_pending_timeout("ch.test", "producer"),
              TransitionEffect::ToDisconnected);
    EXPECT_EQ(r.find_presence("ch.test", "producer"), nullptr)
        << "Wave M3 step 5h: tombstone removal";

    // Row gone → NoChange (idempotent — pre-fix this was 'already
    // Disconnected', same semantic via different mechanism).
    EXPECT_EQ(r.on_pending_timeout("ch.test", "producer"),
              TransitionEffect::NoChange);
}

TEST(RoleEntryApi, OnDereg_ErasesPresenceRow_IdempotentSecondCall)
{
    // Wave M3 step 5h: on_dereg ERASES the presence row.
    RoleEntry r = make_role("prod.cam.uid00000001");
    ASSERT_EQ(r.add_presence("ch.a", "producer"), AddPresenceResult::Created);
    ASSERT_EQ(r.add_presence("ch.b", "producer"), AddPresenceResult::Created);
    r.on_heartbeat("ch.a", "producer", std::chrono::steady_clock::now());

    // Connected → erase.
    EXPECT_EQ(r.on_dereg("ch.a", "producer"),
              TransitionEffect::ToDisconnected);
    EXPECT_EQ(r.find_presence("ch.a", "producer"), nullptr);

    // Row already gone → NoChange.
    EXPECT_EQ(r.on_dereg("ch.a", "producer"),
              TransitionEffect::NoChange);

    // Sibling (ch.b) untouched (still Connected).
    const auto *p_b = r.find_presence("ch.b", "producer");
    ASSERT_NE(p_b, nullptr);
    EXPECT_EQ(p_b->state, RoleState::Connected);
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

// ──────────────────────────────────────────────────────────────────────
// Wave-B M2 (2/3) — consumer-presence FSM through HubState ops.
// Producer side proven by HubStateOps / HubStateProducerPendingTimeout
// above.  These pin the consumer branch added to `_on_heartbeat_timeout`
// and `_on_pending_timeout` (HEP-CORE-0023 §2.1 + §2.1.1).
// ──────────────────────────────────────────────────────────────────────

TEST(HubStateConsumerHeartbeatTimeout, TransitionsConsumerPresenceToPending)
{
    HubState s;

    // Channel registered → default producer "prod.main.test" landed
    // with a producer-presence row.  Separate consumer uid joins to
    // get an isolated consumer-presence row to demote.
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.cons.hb"));
    HubStateTestAccess::on_consumer_joined(s, "ch.cons.hb",
                                            make_consumer("cons.B.uid00000002"));

    // Listener that would fire on a channel-status change.  The
    // contract (HEP-CORE-0023 §2.1) says consumer transitions DO NOT
    // affect ChannelObservable — handler must not fire.
    std::vector<std::string> status_fires;
    auto hid = s.subscribe_channel_status_changed(
        [&](const ChannelEntry &e, ChannelObservable) {
            status_fires.push_back(e.name);
        });
    ASSERT_NE(hid, kInvalidHandlerId);

    // Anchor `last_heartbeat` for the consumer.
    HubStateTestAccess::on_heartbeat(
        s, "ch.cons.hb", "cons.B.uid00000002", "consumer",
        std::chrono::steady_clock::now(), std::nullopt);

    HubStateTestAccess::on_heartbeat_timeout(
        s, "ch.cons.hb", "cons.B.uid00000002", "consumer");

    // Consumer-presence demoted to Pending.
    auto        snap = s.snapshot();
    const auto *pc   = find_presence_in(snap, "ch.cons.hb",
                                          "cons.B.uid00000002", "consumer");
    ASSERT_NE(pc, nullptr);
    EXPECT_EQ(pc->state, RoleState::Pending);

    // Producer-presence untouched.
    const auto *pp = find_presence_in(snap, "ch.cons.hb",
                                       "prod.main.test", "producer");
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(pp->state, RoleState::Connected);

    // Counter bumped — producer + consumer transitions both count
    // toward ready_to_pending_total (per-role-presence FSM metric).
    EXPECT_EQ(s.counters().ready_to_pending_total, 1u);

    // ChannelStatusChangedHandler MUST NOT fire on a consumer-only
    // transition.  Pinning this catches a regression where the
    // producer-side fan-out leaked into the consumer branch.
    EXPECT_TRUE(status_fires.empty())
        << "Consumer-presence Connected→Pending must NOT fire "
           "ChannelStatusChangedHandler (HEP-CORE-0023 §2.1).";

    s.unsubscribe(hid);
}

TEST(HubStateConsumerPendingTimeout, TransitionsToDisconnected_ChannelSurvives)
{
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.cons.pt"));
    HubStateTestAccess::on_consumer_joined(s, "ch.cons.pt",
                                            make_consumer("cons.B.uid00000002"));

    // Drive Connected→Pending first; pending-timeout is a no-op on
    // a Connected presence (TransitionEffect::NoChange).
    HubStateTestAccess::on_heartbeat(
        s, "ch.cons.pt", "cons.B.uid00000002", "consumer",
        std::chrono::steady_clock::now(), std::nullopt);
    HubStateTestAccess::on_heartbeat_timeout(
        s, "ch.cons.pt", "cons.B.uid00000002", "consumer");

    auto pt = HubStateTestAccess::on_pending_timeout(
        s, "ch.cons.pt", "cons.B.uid00000002", "consumer");

    EXPECT_TRUE(pt.removed)
        << "ChannelEntry.consumers[] slot erased for cons.B";
    EXPECT_FALSE(pt.channel_now_empty)
        << "Consumer-presence disconnect MUST NOT mark the channel empty — "
           "producer side owns channel teardown (HEP-CORE-0023 §2.1.1).";

    // Channel still exists with producer-presence intact and the
    // consumer slot gone.
    auto ch = s.channel("ch.cons.pt");
    ASSERT_TRUE(ch.has_value());
    EXPECT_EQ(ch->producer_count(), 1u);
    EXPECT_EQ(ch->consumer_count(), 0u);
    EXPECT_NE(ch->find_producer("prod.main.test"), nullptr);
    EXPECT_EQ(ch->find_consumer("cons.B.uid00000002"), nullptr);

    // Wave M3 step 5h — pending-timeout erases the presence row, so
    // the consumer-presence is no longer findable.
    auto        snap = s.snapshot();
    const auto *pc   = find_presence_in(snap, "ch.cons.pt",
                                          "cons.B.uid00000002", "consumer");
    EXPECT_EQ(pc, nullptr)
        << "Consumer-presence row erased on pending-timeout (M3 step 5h)";

    EXPECT_EQ(s.counters().pending_to_deregistered_total, 1u);
}

TEST(HubStateConsumerPendingTimeout, LastPresence_TriggersRoleDisconnected)
{
    // When a consumer-presence is the role's last alive presence,
    // pending-timeout must dispatch the role-disconnect cascade so the
    // RoleEntry is erased — symmetric with the multi-producer path
    // (Wave M3 step 5b).  Verifies via observable role-disappearance:
    // post-timeout, `s.role(uid)` returns nullopt.
    HubState s;
    HubStateTestAccess::on_channel_registered(s, make_channel("ch.cons.lp"));
    HubStateTestAccess::on_consumer_joined(s, "ch.cons.lp",
                                            make_consumer("cons.B.uid00000002"));

    ASSERT_TRUE(s.role("cons.B.uid00000002").has_value());

    HubStateTestAccess::on_heartbeat(
        s, "ch.cons.lp", "cons.B.uid00000002", "consumer",
        std::chrono::steady_clock::now(), std::nullopt);
    HubStateTestAccess::on_heartbeat_timeout(
        s, "ch.cons.lp", "cons.B.uid00000002", "consumer");
    auto pt = HubStateTestAccess::on_pending_timeout(
        s, "ch.cons.lp", "cons.B.uid00000002", "consumer");

    EXPECT_TRUE(pt.removed);

    // Role's last presence just went Disconnected →
    // _dispatch_role_disconnected_if_dead erases the RoleEntry.
    EXPECT_FALSE(s.role("cons.B.uid00000002").has_value())
        << "Role entry erased after consumer-presence pending-timeout "
           "leaves no alive presence (HEP-CORE-0023 §2.1 + M3 step 5b).";

    // Channel still alive (producer remains).
    EXPECT_TRUE(s.channel("ch.cons.lp").has_value());
}

// ─── HEP-CORE-0036 §4.1 channel-access index — D1 ────────────────────────────

TEST(HubStateChannelAccess, OpenedThenClosed_Roundtrip)
{
    HubState s;

    // Before open: no record.
    EXPECT_FALSE(s.channel_access("ch.auth").has_value());

    HubStateTestAccess::on_channel_access_opened(s, "ch.auth");
    auto a = s.channel_access("ch.auth");
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(a->authorized_consumer_pubkeys.empty());

    HubStateTestAccess::on_channel_access_closed(s, "ch.auth");
    EXPECT_FALSE(s.channel_access("ch.auth").has_value());
}

// ─── Rule-6 retirement: Opened_ShmSecret_Preserved ───────────────────
// HEP-CORE-0041 1i-cleanup S3 (#275, 2026-06-30) — retired.
//
// Pinned `ChannelAccessEntry::shm_secret` round-trip semantics for the
// AUTH-4 / #164 design (broker-minted SHM guard secret carried on
// CONSUMER_REG_ACK).  HEP-CORE-0041 SUPERSEDED that design: SHM auth
// runs on the capability-fd handshake at L2 (§5.5), not a broker-
// minted shared token.  The wire never carried it (substep 1g closed
// CONSUMER_REG_ACK without a `shm_secret` field), the `shm_secret`
// field on `ChannelAccessEntry` was deleted in S3 (#275), and the
// `_on_channel_access_opened` parameter became single-arg.  No
// surviving contract for this test to pin.
//
// ─── Rule-6 retirement: Opened_Idempotent_DoesNotOverwriteShmSecret ─
// Same retirement reason — pinned the idempotent-preservation
// semantics of the now-deleted `shm_secret` field.  The idempotent-
// open semantic for `authorized_consumer_pubkeys` is still pinned by
// `OpenedThenClosed_Roundtrip` and `ConsumerAuthorized_Idempotent`.

TEST(HubStateChannelAccess, ConsumerAuthorized_AddsToAllowlist)
{
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.auth");
    HubStateTestAccess::on_consumer_authorized(s, "ch.auth", "PUB-CONS-A");
    HubStateTestAccess::on_consumer_authorized(s, "ch.auth", "PUB-CONS-B");
    auto a = s.channel_access("ch.auth");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->authorized_consumer_pubkeys.size(), 2u);
    EXPECT_EQ(a->authorized_consumer_pubkeys.count("PUB-CONS-A"), 1u);
    EXPECT_EQ(a->authorized_consumer_pubkeys.count("PUB-CONS-B"), 1u);
}

TEST(HubStateChannelAccess, ConsumerAuthorized_Idempotent)
{
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.auth");
    HubStateTestAccess::on_consumer_authorized(s, "ch.auth", "PUB-A");
    HubStateTestAccess::on_consumer_authorized(s, "ch.auth", "PUB-A");
    auto a = s.channel_access("ch.auth");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->authorized_consumer_pubkeys.size(), 1u);
}

TEST(HubStateChannelAccess, ConsumerRevoked_RemovesFromAllowlist)
{
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.auth");
    HubStateTestAccess::on_consumer_authorized(s, "ch.auth", "PUB-A");
    HubStateTestAccess::on_consumer_authorized(s, "ch.auth", "PUB-B");
    HubStateTestAccess::on_consumer_revoked(s, "ch.auth", "PUB-A");
    auto a = s.channel_access("ch.auth");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->authorized_consumer_pubkeys.size(), 1u);
    EXPECT_EQ(a->authorized_consumer_pubkeys.count("PUB-A"), 0u);
    EXPECT_EQ(a->authorized_consumer_pubkeys.count("PUB-B"), 1u);
}

TEST(HubStateChannelAccess, ConsumerRevoked_Idempotent_NoSuchPubkey)
{
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.auth");
    HubStateTestAccess::on_consumer_authorized(s, "ch.auth", "PUB-A");
    // Revoke a pubkey that was never authorized — no-op.
    HubStateTestAccess::on_consumer_revoked(s, "ch.auth", "PUB-NEVER");
    auto a = s.channel_access("ch.auth");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->authorized_consumer_pubkeys.size(), 1u);
    EXPECT_EQ(a->authorized_consumer_pubkeys.count("PUB-A"), 1u);
}

TEST(HubStateChannelAccess, ConsumerAuthorized_NoOpWithoutChannelAccess)
{
    // Per contract: if no access record exists, authorize/revoke are
    // silently dropped (safe-default per HEP-0036 §I5).
    HubState s;
    HubStateTestAccess::on_consumer_authorized(s, "ch.no.such", "PUB-X");
    EXPECT_FALSE(s.channel_access("ch.no.such").has_value());
}

TEST(HubStateChannelAccess, Close_Idempotent_NoSuchChannel)
{
    HubState s;
    HubStateTestAccess::on_channel_access_closed(s, "ch.no.such"); // no-op, no crash
    EXPECT_FALSE(s.channel_access("ch.no.such").has_value());
}

TEST(HubStateChannelAccess, MultiChannel_Isolated)
{
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.A");
    HubStateTestAccess::on_channel_access_opened(s, "ch.B");
    HubStateTestAccess::on_consumer_authorized(s, "ch.A", "PUB-A1");
    HubStateTestAccess::on_consumer_authorized(s, "ch.B", "PUB-B1");
    HubStateTestAccess::on_consumer_authorized(s, "ch.B", "PUB-B2");

    auto a = s.channel_access("ch.A");
    auto b = s.channel_access("ch.B");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->authorized_consumer_pubkeys.size(), 1u);
    EXPECT_EQ(b->authorized_consumer_pubkeys.size(), 2u);

    // Closing one doesn't affect the other.
    HubStateTestAccess::on_channel_access_closed(s, "ch.A");
    EXPECT_FALSE(s.channel_access("ch.A").has_value());
    EXPECT_TRUE(s.channel_access("ch.B").has_value());
}

TEST(HubStateChannelAccess, InvalidChannelName_BumpsCounterAndNoOp)
{
    HubState s;
    const auto &cnts_before = s.counters().msg_type_counts;
    const auto before =
        cnts_before.count("sys.invalid_identifier_rejected")
            ? cnts_before.at("sys.invalid_identifier_rejected") : 0u;
    HubStateTestAccess::on_channel_access_opened(s, "not a valid channel id");
    const auto &cnts_after = s.counters().msg_type_counts;
    const auto after =
        cnts_after.at("sys.invalid_identifier_rejected");
    EXPECT_EQ(after, before + 1)
        << "Invalid channel identifier must bump counter and no-op.";
    EXPECT_FALSE(s.channel_access("not a valid channel id").has_value());
}

TEST(HubStateChannelAccess, EmptyPubkey_BumpsCounterAndNoOp)
{
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.auth");
    const auto &cnts_before = s.counters().msg_type_counts;
    const auto before =
        cnts_before.count("sys.invalid_identifier_rejected")
            ? cnts_before.at("sys.invalid_identifier_rejected") : 0u;
    HubStateTestAccess::on_consumer_authorized(s, "ch.auth", "");
    const auto &cnts_after = s.counters().msg_type_counts;
    const auto after =
        cnts_after.at("sys.invalid_identifier_rejected");
    EXPECT_EQ(after, before + 1)
        << "Empty pubkey must bump counter and no-op.";
    auto a = s.channel_access("ch.auth");
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(a->authorized_consumer_pubkeys.empty());
}

// ── Audit H4 follow-ons (2026-06-03 close-out 2) ────────────────────────────

TEST(HubStateChannelAccess, ConsumerRevoked_AfterClose_NoOp)
{
    // Safety-net: after `_on_channel_closed`, a `_on_consumer_revoked`
    // arrives (e.g. consumer-PID-death raced with the channel
    // teardown).  Must NOT crash; must NOT bump the invalid-id
    // counter (revoke-without-record is silent safe-default per the
    // mutator contract — `hub_state.cpp:_on_consumer_revoked`).
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.gone");
    HubStateTestAccess::on_consumer_authorized(s, "ch.gone", "PUB-A");
    HubStateTestAccess::on_channel_access_closed(s, "ch.gone");

    const auto &cnts_before = s.counters().msg_type_counts;
    const auto before =
        cnts_before.count("sys.invalid_identifier_rejected")
            ? cnts_before.at("sys.invalid_identifier_rejected") : 0u;
    HubStateTestAccess::on_consumer_revoked(s, "ch.gone", "PUB-A");
    const auto &cnts_after = s.counters().msg_type_counts;
    const auto after =
        cnts_after.count("sys.invalid_identifier_rejected")
            ? cnts_after.at("sys.invalid_identifier_rejected") : 0u;
    EXPECT_EQ(after, before)
        << "revoke-after-close must be silent (no counter bump)";
    EXPECT_FALSE(s.channel_access("ch.gone").has_value());
}

TEST(HubStateChannelAccess, ConsumerAuthorized_AfterClose_DoesNotResurrect)
{
    // A regression where `_on_consumer_authorized` started
    // `try_emplace`-ing the record (instead of `find` then
    // early-return) would silently resurrect a torn-down channel's
    // allowlist.  Pin the documented contract: authorize without
    // record is a no-op.
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.gone");
    HubStateTestAccess::on_consumer_authorized(s, "ch.gone", "PUB-A");
    HubStateTestAccess::on_channel_access_closed(s, "ch.gone");
    HubStateTestAccess::on_consumer_authorized(s, "ch.gone", "PUB-B");
    EXPECT_FALSE(s.channel_access("ch.gone").has_value())
        << "authorize-after-close MUST NOT resurrect the channel record";
}

TEST(HubStateChannelAccess, EmptyPubkey_Revoke_BumpsCounter)
{
    // Symmetric to `EmptyPubkey_BumpsCounterAndNoOp`: the revoke
    // path also rejects empty pubkey with a counter bump.  A
    // regression that silently dropped empty pubkey from
    // `_on_consumer_revoked` (no bump) would slip through without
    // this test.
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.auth");
    HubStateTestAccess::on_consumer_authorized(s, "ch.auth", "PUB-A");
    const auto &cnts_before = s.counters().msg_type_counts;
    const auto before =
        cnts_before.count("sys.invalid_identifier_rejected")
            ? cnts_before.at("sys.invalid_identifier_rejected") : 0u;
    HubStateTestAccess::on_consumer_revoked(s, "ch.auth", "");
    const auto &cnts_after = s.counters().msg_type_counts;
    const auto after =
        cnts_after.at("sys.invalid_identifier_rejected");
    EXPECT_EQ(after, before + 1);
    // Allowlist unchanged.
    auto a = s.channel_access("ch.auth");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->authorized_consumer_pubkeys.count("PUB-A"), 1u);
}

TEST(HubStateChannelAccess, InvalidChannel_AllFourMutators_BumpCounter)
{
    // Mutation-sweep: all four mutators must validate the channel
    // identifier at the op-entry boundary (§G2.2.0b).  The original
    // `InvalidChannelName_BumpsCounterAndNoOp` test only exercised
    // `_on_channel_access_opened`; a regression that dropped
    // validation from any of the other three mutators would slip
    // through.
    HubState s;
    const std::string bad = "not a valid channel id";
    const auto base = [&]() {
        const auto &c = s.counters().msg_type_counts;
        return c.count("sys.invalid_identifier_rejected")
                   ? c.at("sys.invalid_identifier_rejected") : 0u;
    };
    const auto b0 = base();
    HubStateTestAccess::on_channel_access_opened(s, bad);
    EXPECT_EQ(base(), b0 + 1);
    HubStateTestAccess::on_channel_access_closed(s, bad);
    EXPECT_EQ(base(), b0 + 2);
    HubStateTestAccess::on_consumer_authorized(s, bad, "PUB-A");
    EXPECT_EQ(base(), b0 + 3);
    HubStateTestAccess::on_consumer_revoked(s, bad, "PUB-A");
    EXPECT_EQ(base(), b0 + 4);
    EXPECT_FALSE(s.channel_access(bad).has_value());
}

TEST(HubStateChannelAccess, MultiChannel_SamePubkey_RevokeIsScoped)
{
    // A regression where the allowlist were accidentally a
    // process-global set (instead of per-channel) would not be
    // caught by `MultiChannel_Isolated` (which uses distinct pubkeys
    // per channel).  This test uses the SAME pubkey on two channels
    // and verifies revoke on one doesn't leak to the other.
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.A");
    HubStateTestAccess::on_channel_access_opened(s, "ch.B");
    HubStateTestAccess::on_consumer_authorized(s, "ch.A", "PUB-SAME");
    HubStateTestAccess::on_consumer_authorized(s, "ch.B", "PUB-SAME");
    HubStateTestAccess::on_consumer_revoked(s, "ch.A", "PUB-SAME");

    auto a = s.channel_access("ch.A");
    auto b = s.channel_access("ch.B");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->authorized_consumer_pubkeys.count("PUB-SAME"), 0u);
    EXPECT_EQ(b->authorized_consumer_pubkeys.count("PUB-SAME"), 1u)
        << "revoke on ch.A leaked into ch.B — allowlist is not per-channel";
}

// ═══ HEP-CORE-0042 §5.2 state — channel_version / confirmed_version / instance
// ═══════════════════════════════════════════════════════════════════════════
//
// State-level tests for the Channel Attach Coordination Protocol
// (task #246 Phase 2.2, 2026-07-01).  The three counters exercised here
// underpin the ZMQ pre-attach fast-path in
// `BrokerServiceImpl::handle_consumer_attach_req_zmq` and the drain
// semantics in `handle_channel_auth_applied_req`.  Handler-level flow is
// covered at L3 (Phase 2.3/2.4); this block pins the pure state
// mutations directly on HubState so any future refactor that regresses
// the counter contract fails here first.

TEST(HubStateHep0042, ChannelVersion_BumpsOnDistinctAuthorize)
{
    // §5.2: channel_version bumps on ANY mutation to
    // authorized_consumer_pubkeys.  Two distinct pubkeys => two bumps.
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.hep42");

    auto v0 = s.channel_access("ch.hep42");
    ASSERT_TRUE(v0.has_value());
    EXPECT_EQ(v0->channel_version, 0u) << "initial channel_version must be 0";

    HubStateTestAccess::on_consumer_authorized(s, "ch.hep42", "PUB-A");
    auto v1 = s.channel_access("ch.hep42");
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(v1->channel_version, 1u);

    HubStateTestAccess::on_consumer_authorized(s, "ch.hep42", "PUB-B");
    auto v2 = s.channel_access("ch.hep42");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->channel_version, 2u);
}

TEST(HubStateHep0042, ChannelVersion_NoBumpOnDuplicateAuthorize)
{
    // §5.2: idempotent re-authorize does NOT bump channel_version —
    // pubkey already in the set is a no-op mutation.  A false bump
    // here would force spurious wait-path cycles on downstream
    // consumers and inflate the P1 accepted cost.
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.hep42");
    HubStateTestAccess::on_consumer_authorized(s, "ch.hep42", "PUB-DUP");
    auto v1 = s.channel_access("ch.hep42");
    ASSERT_TRUE(v1.has_value());
    ASSERT_EQ(v1->channel_version, 1u);

    HubStateTestAccess::on_consumer_authorized(s, "ch.hep42", "PUB-DUP");
    auto v2 = s.channel_access("ch.hep42");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->channel_version, 1u)
        << "idempotent re-authorize must not bump channel_version";
}

TEST(HubStateHep0042, ChannelVersion_BumpsOnRevoke)
{
    // §5.2: revoke that actually removes an entry bumps channel_version.
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.hep42");
    HubStateTestAccess::on_consumer_authorized(s, "ch.hep42", "PUB-X");
    auto v1 = s.channel_access("ch.hep42");
    ASSERT_TRUE(v1.has_value());
    ASSERT_EQ(v1->channel_version, 1u);

    HubStateTestAccess::on_consumer_revoked(s, "ch.hep42", "PUB-X");
    auto v2 = s.channel_access("ch.hep42");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->channel_version, 2u);
}

TEST(HubStateHep0042, ChannelVersion_NoBumpOnMissingRevoke)
{
    // §5.2: revoking a pubkey not present is a no-op and must not
    // bump.  Symmetric with the idempotent-authorize invariant.
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.hep42");
    auto v0 = s.channel_access("ch.hep42");
    ASSERT_TRUE(v0.has_value());
    ASSERT_EQ(v0->channel_version, 0u);

    HubStateTestAccess::on_consumer_revoked(s, "ch.hep42", "PUB-NOT-THERE");
    auto v1 = s.channel_access("ch.hep42");
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(v1->channel_version, 0u);
}

TEST(HubStateHep0042, ProducerInstance_BumpsOnFirstRegistration)
{
    // §5.2: producer_instance starts at 0 for a never-seen role_uid.
    // First _on_producer_added lifts to 1.  Broker echoes this to
    // producer in PRODUCER_REG_ACK per §4.2a.
    HubState s;
    EXPECT_EQ(s.producer_instance("prod.first.uid00000001"), 0u)
        << "unseen producer must read as instance 0";

    auto r = HubStateTestAccess::on_producer_added(
        s, "ch.hep42.instance",
        make_schema_invariants(),
        make_zmq_transport(),
        make_producer("prod.first.uid00000001", 1001));
    ASSERT_EQ(r.producer_result, AddProducerResult::Created);

    EXPECT_EQ(s.producer_instance("prod.first.uid00000001"), 1u)
        << "first registration must lift instance 0 → 1";
}

TEST(HubStateHep0042, ProducerInstance_BumpsAcrossMultiChannelRegistration)
{
    // Instance is CHANNEL-INDEPENDENT (keyed by role_uid alone).  A
    // producer serving two channels bumps its instance twice — once
    // per _on_producer_added.  This pins the per-role_uid semantics of
    // the counter (as opposed to per-(channel, producer)).
    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.hep42.multi.a",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.multi.uid00000001", 2002))
                  .producer_result,
              AddProducerResult::Created);
    ASSERT_EQ(s.producer_instance("prod.multi.uid00000001"), 1u);

    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, "ch.hep42.multi.b",
                  make_schema_invariants(),
                  make_zmq_transport(),
                  make_producer("prod.multi.uid00000001", 2002))
                  .producer_result,
              AddProducerResult::Created);
    EXPECT_EQ(s.producer_instance("prod.multi.uid00000001"), 2u);
}

TEST(HubStateHep0042, ProducerInstance_BumpsOnCrashRestartSameChannel)
{
    // §5.2 P4 stale-instance guard — the real crash-restart scenario:
    // producer runs on channel K, gets reaped, new instance re-adds
    // to K.  The counter must bump on the re-add so the OLD instance's
    // in-flight APPLIED_REQ (echoing instance=1) gets dropped when
    // the new instance is at 2.
    //
    // Simulated via _on_producer_dropped (reap) + _on_producer_added.
    // add_producer's UidConflict short-circuit is NOT hit here because
    // the reap already emptied producers[].
    const std::string uid = "prod.crash.uid00000001";
    const std::string ch  = "ch.hep42.crash";

    HubState s;
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, ch, make_schema_invariants(), make_zmq_transport(),
                  make_producer(uid, 3001))
                  .producer_result,
              AddProducerResult::Created);
    ASSERT_EQ(s.producer_instance(uid), 1u);

    // Add a second producer so the reap of `uid` is NOT the last-
    // producer path (which would nuke the whole channel via atomic
    // teardown).  This mirrors the fan-in production case: one
    // producer crashes while another survives.
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, ch, make_schema_invariants(), make_zmq_transport(),
                  make_producer("prod.sibling.uid00000002", 3002))
                  .producer_result,
              AddProducerResult::Created);

    auto rm = HubStateTestAccess::on_producer_dropped(
        s, ch, uid, pylabhub::hub::ChannelCloseReason::VoluntaryDereg);
    ASSERT_TRUE(rm.removed);
    ASSERT_FALSE(rm.channel_now_empty);

    // Re-add the same uid — the counter must bump.
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, ch, make_schema_invariants(), make_zmq_transport(),
                  make_producer(uid, 3001))
                  .producer_result,
              AddProducerResult::Created);
    EXPECT_EQ(s.producer_instance(uid), 2u)
        << "crash-restart on same channel must bump instance (§5.2)";
}

TEST(HubStateHep0042, ConfirmedVersion_ResetOnReRegistration)
{
    // §5.4 mandates confirmed_version[K][P] = 0 on re-registration.
    // Without this, the new instance inherits the OLD instance's
    // confirmed state — fast-path admits against an empty ZAP cache,
    // CURVE handshake fails, and P1 ("one wait-path cycle per rare
    // event") is broken until the next channel_version bump.
    const std::string uid = "prod.reset.uid00000001";
    const std::string ch  = "ch.hep42.reset";

    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, ch);

    // Register + advance confirmed to 42.
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, ch, make_schema_invariants(), make_zmq_transport(),
                  make_producer(uid, 4001))
                  .producer_result,
              AddProducerResult::Created);
    ASSERT_EQ(s._on_producer_confirmed(ch, uid, 42), 42u);

    // Sibling to keep the channel alive across the reap.
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, ch, make_schema_invariants(), make_zmq_transport(),
                  make_producer("prod.sibling.reset.uid00000002", 4002))
                  .producer_result,
              AddProducerResult::Created);

    auto rm = HubStateTestAccess::on_producer_dropped(
        s, ch, uid, pylabhub::hub::ChannelCloseReason::VoluntaryDereg);
    ASSERT_TRUE(rm.removed);

    // Verify confirmed_version cleared after the drop.
    auto after_drop = s.channel_access(ch);
    ASSERT_TRUE(after_drop.has_value());
    EXPECT_EQ(after_drop->confirmed_version_per_producer.count(uid), 0u)
        << "producer drop must erase confirmed_version[K][P]";

    // Re-register the crashed uid.  Instance bumps 1 → 2 AND
    // confirmed_version stays at 0 for this pair.
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, ch, make_schema_invariants(), make_zmq_transport(),
                  make_producer(uid, 4001))
                  .producer_result,
              AddProducerResult::Created);
    auto after_readd = s.channel_access(ch);
    ASSERT_TRUE(after_readd.has_value());
    auto it = after_readd->confirmed_version_per_producer.find(uid);
    if (it != after_readd->confirmed_version_per_producer.end())
        FAIL() << "confirmed_version must be reset (map entry erased) "
                  "on re-registration; instead saw " << it->second;
}

TEST(HubStateHep0042, ChannelAccess_ErasedByChannelAccessClosed)
{
    // Pin the broker-side teardown-symmetry invariant enforced across
    // three call sites in `broker_service.cpp` (VoluntaryDereg last-
    // producer, HeartbeatTimeout last-producer, script-requested close):
    // the caller MUST invoke `_on_channel_access_closed(K)` when the
    // channel is torn down, otherwise the entire ChannelAccessEntry —
    // including channel_version + confirmed_version_per_producer —
    // leaks into `channel_access_index`.  If a subsequent channel with
    // the same name is later opened, it inherits stale state that a
    // fast-path check would treat as up-to-date.
    //
    // This L2 test pins the HubState primitive (idempotent erase) so
    // any future test that goes through the broker teardown path can
    // ASSERT_FALSE(channel_access(K).has_value()) with confidence in
    // what "erased" means.
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.hep42.teardown");
    HubStateTestAccess::on_consumer_authorized(s, "ch.hep42.teardown", "PUB-A");
    auto before = s.channel_access("ch.hep42.teardown");
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->channel_version, 1u);
    ASSERT_EQ(before->authorized_consumer_pubkeys.count("PUB-A"), 1u);

    HubStateTestAccess::on_channel_access_closed(s, "ch.hep42.teardown");
    EXPECT_FALSE(s.channel_access("ch.hep42.teardown").has_value())
        << "teardown must drop the entire ChannelAccessEntry (not just "
           "reset its fields) so a same-named re-open starts clean";

    // Idempotent: a second call is a silent no-op (safe for the
    // symmetry contract's "call it on every teardown path" wording,
    // which permits double-invocation across race paths).
    HubStateTestAccess::on_channel_access_closed(s, "ch.hep42.teardown");
    EXPECT_FALSE(s.channel_access("ch.hep42.teardown").has_value());

    // Re-open under the same name must start with fresh state (no
    // leaked channel_version or authorized pubkeys).
    HubStateTestAccess::on_channel_access_opened(s, "ch.hep42.teardown");
    auto after = s.channel_access("ch.hep42.teardown");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->channel_version, 0u);
    EXPECT_EQ(after->authorized_consumer_pubkeys.size(), 0u);
    EXPECT_EQ(after->confirmed_version_per_producer.size(), 0u);
}

TEST(HubStateHep0042, ConfirmedVersion_ResetOnHeartbeatTimeout)
{
    // §5.4: kDead heartbeat is a normative reset trigger.  Pin the
    // reset in _on_pending_timeout (multi-producer non-last branch)
    // so a producer that times out doesn't leak stale
    // confirmed_version state into a subsequent re-registration.
    //
    // Two producers on the same channel; A gets driven Connected →
    // Pending → Disconnected via the heartbeat FSM; the surviving
    // sibling keeps the channel alive so the drop enters the non-
    // last-producer branch (parallel to _on_producer_dropped's
    // VoluntaryDereg branch).
    const std::string uid_a = "prod.hbtimeout.a.uid00000001";
    const std::string uid_b = "prod.hbtimeout.b.uid00000002";
    const std::string ch    = "ch.hep42.hbtimeout";

    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, ch);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, ch, make_schema_invariants(), make_zmq_transport(),
                  make_producer(uid_a, 5001))
                  .producer_result,
              AddProducerResult::Created);
    ASSERT_EQ(HubStateTestAccess::on_producer_added(
                  s, ch, make_schema_invariants(), make_zmq_transport(),
                  make_producer(uid_b, 5002))
                  .producer_result,
              AddProducerResult::Created);

    // A applies allowlist v42 — the state that the timeout must clear.
    ASSERT_EQ(s._on_producer_confirmed(ch, uid_a, 42), 42u);

    // First heartbeat for both so the FSM has a Connected state to
    // transition FROM.
    const auto now = std::chrono::steady_clock::now();
    HubStateTestAccess::on_heartbeat(s, ch, uid_a, "producer", now,
                                      std::nullopt);
    HubStateTestAccess::on_heartbeat(s, ch, uid_b, "producer", now,
                                      std::nullopt);

    // A → Pending → Disconnected via heartbeat FSM.
    HubStateTestAccess::on_heartbeat_timeout(s, ch, uid_a);
    auto pt = HubStateTestAccess::on_pending_timeout(s, ch, uid_a);
    ASSERT_TRUE(pt.removed);
    ASSERT_FALSE(pt.channel_now_empty);

    // A's confirmed_version[K][P] must have been erased alongside the
    // producer removal.
    auto after = s.channel_access(ch);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->confirmed_version_per_producer.count(uid_a), 0u)
        << "kDead (heartbeat-timeout) must erase confirmed_version[K][P] "
           "(HEP-CORE-0042 §5.4 kDead reset trigger)";
    // Sanity: didn't clobber B's state (B was never confirmed here, but
    // the count must be 0 for the un-confirmed uid too — i.e., no
    // spurious side-effect on the sibling).
    EXPECT_EQ(after->confirmed_version_per_producer.count(uid_b), 0u);
}

TEST(HubStateHep0042, ProducerConfirmed_AdvancesConfirmedVersion)
{
    // §5.4 step c: _on_producer_confirmed advances
    // confirmed_version[K][P] to max(current, applied).  Read-back via
    // channel_access() reflects the new value.
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.hep42.confirm");

    const auto ret =
        s._on_producer_confirmed("ch.hep42.confirm", "prod.confirm.uid1", 42);
    EXPECT_EQ(ret, 42u);

    auto access = s.channel_access("ch.hep42.confirm");
    ASSERT_TRUE(access.has_value());
    auto it = access->confirmed_version_per_producer.find(
        "prod.confirm.uid1");
    ASSERT_NE(it, access->confirmed_version_per_producer.end());
    EXPECT_EQ(it->second, 42u);
}

TEST(HubStateHep0042, ProducerConfirmed_MonotonicMax)
{
    // §5.4 step c wording "= max(current, applied)": a lower incoming
    // applied_version must NOT regress the stored confirmed_version.
    // Guards against out-of-order APPLIED_REQ arrivals from the SAME
    // producer instance (network reorder).
    HubState s;
    HubStateTestAccess::on_channel_access_opened(s, "ch.hep42.mono");

    EXPECT_EQ(s._on_producer_confirmed("ch.hep42.mono", "prod.mono.uid1", 10),
              10u);
    // Out-of-order lower version — no regression.
    EXPECT_EQ(s._on_producer_confirmed("ch.hep42.mono", "prod.mono.uid1", 5),
              10u);
    // Same-version idempotent.
    EXPECT_EQ(s._on_producer_confirmed("ch.hep42.mono", "prod.mono.uid1", 10),
              10u);
    // Higher advances.
    EXPECT_EQ(s._on_producer_confirmed("ch.hep42.mono", "prod.mono.uid1", 15),
              15u);
}

TEST(HubStateHep0042, ProducerConfirmed_NoOpOnMissingChannel)
{
    // §5.4 (defensive): _on_producer_confirmed for a channel with no
    // access record returns 0 and mutates nothing.  Guards against
    // typo channels reaching the storage layer.
    HubState s;
    EXPECT_EQ(s._on_producer_confirmed("ch.hep42.does-not-exist",
                                        "prod.p1.uid1", 99),
              0u);
    // Verify no access record magically appeared.
    EXPECT_FALSE(s.channel_access("ch.hep42.does-not-exist").has_value());
}

// 2026-07-08 topology migration — Phase B slice 7 unit tests for the
// admission-path helpers.  Tech draft §5.1 rule 4 + HEP-CORE-0017 §3.3.0.

TEST(ChannelTopology, ParseWireValues)
{
    EXPECT_EQ(parse_channel_topology("fan-in"),     ChannelTopology::FanIn);
    EXPECT_EQ(parse_channel_topology("fan-out"),    ChannelTopology::FanOut);
    EXPECT_EQ(parse_channel_topology("one-to-one"), ChannelTopology::OneToOne);
    EXPECT_FALSE(parse_channel_topology(""));
    EXPECT_FALSE(parse_channel_topology("FanIn"));         // case-sensitive
    EXPECT_FALSE(parse_channel_topology("fan_in"));        // underscore vs dash
    EXPECT_FALSE(parse_channel_topology("garbage"));
}

TEST(ChannelTopology, ToStringRoundTrip)
{
    for (auto t : {ChannelTopology::FanIn,
                    ChannelTopology::FanOut,
                    ChannelTopology::OneToOne})
    {
        EXPECT_EQ(parse_channel_topology(to_string(t)), t);
    }
}

TEST(ChannelTopology, TransportCompatibility)
{
    // HEP-CORE-0017 §3.3.0 decision matrix: only fan-in × shm is rejected.
    EXPECT_TRUE (transport_topology_compatible(ChannelTopology::FanIn,    "zmq"));
    EXPECT_FALSE(transport_topology_compatible(ChannelTopology::FanIn,    "shm"));
    EXPECT_TRUE (transport_topology_compatible(ChannelTopology::FanOut,   "zmq"));
    EXPECT_TRUE (transport_topology_compatible(ChannelTopology::FanOut,   "shm"));
    EXPECT_TRUE (transport_topology_compatible(ChannelTopology::OneToOne, "zmq"));
    EXPECT_TRUE (transport_topology_compatible(ChannelTopology::OneToOne, "shm"));
    // Unknown transports rejected defensively.
    EXPECT_FALSE(transport_topology_compatible(ChannelTopology::FanOut,   ""));
    EXPECT_FALSE(transport_topology_compatible(ChannelTopology::FanOut,   "tcp"));
}

TEST(ChannelTopology, CheckAgainstStored_InheritOnEmpty)
{
    // Empty incoming → always OK, no mismatch check.  Tech draft §5.1
    // rule 4 branch 3 — defaulted role opts into stored topology.
    EXPECT_EQ(nullptr, check_topology_against_stored(ChannelTopology::FanIn,    ""));
    EXPECT_EQ(nullptr, check_topology_against_stored(ChannelTopology::FanOut,   ""));
    EXPECT_EQ(nullptr, check_topology_against_stored(ChannelTopology::OneToOne, ""));
}

TEST(ChannelTopology, CheckAgainstStored_MatchOnEqual)
{
    // Non-empty matching → OK.
    EXPECT_EQ(nullptr, check_topology_against_stored(ChannelTopology::FanIn,    "fan-in"));
    EXPECT_EQ(nullptr, check_topology_against_stored(ChannelTopology::FanOut,   "fan-out"));
    EXPECT_EQ(nullptr, check_topology_against_stored(ChannelTopology::OneToOne, "one-to-one"));
}

TEST(ChannelTopology, CheckAgainstStored_MismatchFires)
{
    // Non-empty non-matching → TOPOLOGY_MISMATCH.
    EXPECT_STREQ("TOPOLOGY_MISMATCH",
                 check_topology_against_stored(ChannelTopology::FanIn,    "fan-out"));
    EXPECT_STREQ("TOPOLOGY_MISMATCH",
                 check_topology_against_stored(ChannelTopology::FanOut,   "one-to-one"));
    EXPECT_STREQ("TOPOLOGY_MISMATCH",
                 check_topology_against_stored(ChannelTopology::OneToOne, "fan-in"));
}

TEST(ChannelTopology, CheckAgainstStored_InvalidWireValue)
{
    // Non-empty non-parseable → INVALID_REQUEST.  Distinguishes from
    // TOPOLOGY_MISMATCH so the broker can return the right error code.
    EXPECT_STREQ("INVALID_REQUEST",
                 check_topology_against_stored(ChannelTopology::FanIn,    "garbage"));
    EXPECT_STREQ("INVALID_REQUEST",
                 check_topology_against_stored(ChannelTopology::OneToOne, "FanIn"));
    EXPECT_STREQ("INVALID_REQUEST",
                 check_topology_against_stored(ChannelTopology::FanOut,   "one_to_one"));
}

// Cardinality gate — slice 8.

TEST(ChannelTopology, Cardinality_FanIn_AdmitsProducers)
{
    // Fan-in permits N producers.  Producer REG_REQ always admitted
    // regardless of existing producer count.
    for (std::size_t p = 0; p < 8; ++p)
    {
        EXPECT_EQ(nullptr,
                  check_cardinality_admission(ChannelTopology::FanIn,
                                              /*is_consumer_reg=*/false,
                                              /*existing_producers=*/p,
                                              /*existing_consumers=*/0));
    }
}

TEST(ChannelTopology, Cardinality_FanIn_FirstConsumerAdmittedSecondRejected)
{
    // Fan-in permits exactly 1 consumer.
    EXPECT_EQ(nullptr,
              check_cardinality_admission(ChannelTopology::FanIn, true, 0, 0));
    EXPECT_STREQ("FAN_IN_IS_SINGLE_CONSUMER",
                 check_cardinality_admission(ChannelTopology::FanIn, true, 0, 1));
    EXPECT_STREQ("FAN_IN_IS_SINGLE_CONSUMER",
                 check_cardinality_admission(ChannelTopology::FanIn, true, 5, 1));
}

TEST(ChannelTopology, Cardinality_FanOut_AdmitsConsumers)
{
    // Fan-out permits N consumers.  Consumer CONSUMER_REG_REQ always
    // admitted regardless of existing consumer count.
    for (std::size_t c = 0; c < 8; ++c)
    {
        EXPECT_EQ(nullptr,
                  check_cardinality_admission(ChannelTopology::FanOut,
                                              /*is_consumer_reg=*/true,
                                              /*existing_producers=*/1,
                                              /*existing_consumers=*/c));
    }
}

TEST(ChannelTopology, Cardinality_FanOut_FirstProducerAdmittedSecondRejected)
{
    EXPECT_EQ(nullptr,
              check_cardinality_admission(ChannelTopology::FanOut, false, 0, 0));
    EXPECT_STREQ("FAN_OUT_IS_SINGLE_PRODUCER",
                 check_cardinality_admission(ChannelTopology::FanOut, false, 1, 0));
    EXPECT_STREQ("FAN_OUT_IS_SINGLE_PRODUCER",
                 check_cardinality_admission(ChannelTopology::FanOut, false, 1, 3));
}

TEST(ChannelTopology, Cardinality_OneToOne_BothSidesCardinalityOne)
{
    // Both first REG and first CONSUMER_REG admitted.
    EXPECT_EQ(nullptr,
              check_cardinality_admission(ChannelTopology::OneToOne, false, 0, 0));
    EXPECT_EQ(nullptr,
              check_cardinality_admission(ChannelTopology::OneToOne, true, 0, 0));
    EXPECT_EQ(nullptr,
              check_cardinality_admission(ChannelTopology::OneToOne, true, 1, 0));
    EXPECT_EQ(nullptr,
              check_cardinality_admission(ChannelTopology::OneToOne, false, 0, 1));

    // Second producer rejected.
    EXPECT_STREQ("ONE_TO_ONE_CARDINALITY_VIOLATED",
                 check_cardinality_admission(ChannelTopology::OneToOne, false, 1, 0));
    EXPECT_STREQ("ONE_TO_ONE_CARDINALITY_VIOLATED",
                 check_cardinality_admission(ChannelTopology::OneToOne, false, 1, 1));

    // Second consumer rejected.
    EXPECT_STREQ("ONE_TO_ONE_CARDINALITY_VIOLATED",
                 check_cardinality_admission(ChannelTopology::OneToOne, true, 0, 1));
    EXPECT_STREQ("ONE_TO_ONE_CARDINALITY_VIOLATED",
                 check_cardinality_admission(ChannelTopology::OneToOne, true, 1, 1));
}

