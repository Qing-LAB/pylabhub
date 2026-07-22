/**
 * @file test_role_handler.cpp
 * @brief L2 unit tests for `RoleHandler` single-class behavior.
 *
 * Coverage:
 *   - data-structure: presence enumeration, dedup correctness,
 *     channel index lookup, lifetime invariants, duplicate-channel
 *     defensive log;
 *   - start/stop lifecycle: `start_connections()` / `stop_connections()`
 *     state flags, BRC allocation, idempotency, double-start
 *     rejection, post-start routing-primitive pointer identity
 *     (`brc_for_channel/role/band`).
 *
 * The M4 tests were RE-LAYERED here from L3
 * `test_datahub_role_state_machine.cpp` by the AUTH-6 layer-fit audit
 * (`docs/code_review/REVIEW_AUTH6_TestDisposition_2026-06-27.md` §2
 * addendum, 2026-06-27).  They belong at L2 because
 * `BRC::is_connected()` reads the BRC's internal `connected` flag set
 * at the end of `connect()` — BEFORE any wire handshake.  The L3
 * placement required a real broker as scaffolding even though no
 * broker-side protocol behavior was asserted; here the broker
 * endpoint can be any reachable TCP address (ZMQ DEALER connect is
 * non-blocking and lazy).  Real-broker protocol assertions stay at
 * L3.
 *
 * Pattern selection: **Pattern 1+** (`BinaryLifecycleEnvironment` via
 * `PLH_BINARY_LIFECYCLE_MODULES`).  `RoleHandler::build_channel_index_`
 * emits `LOGGER_ERROR` on a duplicate-channel input — Logger is a
 * lifecycle-backed module per HEP-CORE-0001, so any test of code that
 * can emit `LOGGER_*` needs the binary-wide guard.  See
 * `docs/README/README_testing.md § "Pattern 1+"` for the rationale +
 * the decision checklist (Q1: "Does my test call any `LOGGER_*` macro?
 * (Logger is lifecycle-backed.)").  M4 tests also need
 * `crypto::GetLifecycleModule` + `hub::GetZMQContextModule` because
 * `BRC::connect()` constructs a CURVE-wired DEALER socket on the
 * process ZMQ context and reads its seckey from `secure().keys()` via
 * `kRoleIdentityName` (HEP-CORE-0040 §172).
 *
 * Compiled as its own executable `test_layer2_role_handler` — NOT
 * part of any aggregate target — because `BinaryLifecycleEnvironment`
 * requires being the only `LifecycleGuard` owner in its binary.
 */

#include "binary_lifecycle.h"
#include "curve_test_setup.h" // gen_curve_keypair
#include "plh_service.hpp"    // pulls hub::GetZMQContextModule
#include "utils/logger.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_handler.hpp"
#include "utils/role_host_core.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// Binary-wide LifecycleGuard for the 3 modules the M4 lifecycle tests
// need (Logger / crypto / ZMQ context).  Logger alone was sufficient
// for the M3 data-structure tests; M4 tests construct real BRCs via
// `RoleHandler::start_connections()`.  This is the only
// `LifecycleGuard` owner in this binary by design — see file header.
PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule(),
                             pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
                             pylabhub::hub::GetZMQContextModule())

#include <string>
#include <vector>

using pylabhub::config::HubRefConfig;
using pylabhub::scripting::HubConnection;
using pylabhub::scripting::Presence;
using pylabhub::scripting::RoleHandler;
using pylabhub::scripting::RoleKind;
using pylabhub::scripting::to_wire_string;

namespace
{

HubRefConfig make_hub(const std::string &endpoint, const std::string &pubkey = "test-pubkey-aaaa")
{
    HubRefConfig h;
    h.broker = endpoint;
    h.broker_pubkey = pubkey;
    return h;
}

Presence make_presence(const std::string &channel, RoleKind kind, HubRefConfig hub)
{
    Presence p;
    p.hub = std::move(hub);
    p.channel = channel;
    p.role_kind = kind;
    return p;
}

} // namespace

// ── KeyStore identity seeding for M4 lifecycle tests ──────────────────────
//
// `RoleHandler::start_connections(api)` constructs a `BRC::Config` for
// each connection and calls `brc.connect(cfg)`.  `connect()` reads the
// role's seckey from `secure().keys()` via `kRoleIdentityName` per
// HEP-CORE-0040 §172; an unseeded store causes `connect()` to return
// false and `start_connections()` to fail.  Seed once per binary,
// mirroring the pattern in `test_hub_zmq_queue.cpp` lines 109-152.
//
// One identity per process per HEP-CORE-0040 §5.3 — fine here because
// these tests run as a single L2 binary.  Setup runs once via a
// Pattern-1+ `BinaryLifecycleEnvironment`; teardown happens at the
// binary's LifecycleGuard.
namespace
{

class RoleHandlerTestEnvironment : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        namespace sec = pylabhub::utils::security;
        // SMS+KeyStore are brought up by PLH_BINARY_LIFECYCLE_MODULES
        // above via SecureSubsystem::GetLifecycleModule() (HEP-CORE-0043
        // §2.2 — KeyStore is a member of SMS).  This SetUp only seeds
        // the canonical role identity used by tests.
        auto &ks = sec::secure().keys();
        if (!ks.has(sec::kRoleIdentityName))
        {
            const auto kp = pylabhub::tests::gen_curve_keypair();
            ks.add_identity_from_z85(sec::kRoleIdentityName, kp.public_z85, kp.secret_z85);
        }
    }
};

[[maybe_unused]] static auto *kRoleHandlerTestEnv =
    ::testing::AddGlobalTestEnvironment(new RoleHandlerTestEnvironment);

// Returns a `HubRefConfig` with the seeded test identity's pubkey as
// broker pubkey.  `BrokerRequestComm::connect()` rejects empty / non-
// Z85 broker pubkey (#172 strict-CURVE), so the M3 helper
// `make_hub(endpoint)` default `"test-pubkey-aaaa"` is not
// `start_connections()`-compatible.  CURVE handshake itself never
// completes in these tests (no real broker is on the other end of
// the TCP socket), but `connect()` succeeds at the API boundary —
// which is what `BRC::is_connected()` reports.
HubRefConfig make_curve_hub(const std::string &endpoint)
{
    namespace sec = pylabhub::utils::security;
    HubRefConfig h;
    h.broker = endpoint;
    h.broker_pubkey = std::string{sec::secure().keys().pubkey(sec::kRoleIdentityName)};
    return h;
}

} // namespace

// ── RoleKind wire-string round-trip ─────────────────────────────────────────

TEST(RoleKindWire, ToString)
{
    EXPECT_STREQ(to_wire_string(RoleKind::Producer), "producer");
    EXPECT_STREQ(to_wire_string(RoleKind::Consumer), "consumer");
}

// ── Single-presence (producer/consumer) topologies ──────────────────────────

TEST(RoleHandlerSinglePresence, Producer_OneConnection)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("test.out", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 1u);
    EXPECT_EQ(h.connection_count(), 1u);

    const auto *p = h.find_presence_for_channel("test.out");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->channel, "test.out");
    EXPECT_EQ(p->role_kind, RoleKind::Producer);
    EXPECT_NE(p->connection, nullptr)
        << "build_connections_ must wire the presence's connection pointer";
    EXPECT_EQ(p->connection->broker_endpoint, "tcp://127.0.0.1:5570");
}

TEST(RoleHandlerSinglePresence, Consumer_OneConnection)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("test.in", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5570")));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 1u);
    EXPECT_EQ(h.connection_count(), 1u);
    ASSERT_NE(h.find_presence_for_channel("test.in"), nullptr);
    EXPECT_EQ(h.find_presence_for_channel("test.in")->role_kind, RoleKind::Consumer);
}

// ── Processor topologies — the dedup payoff ────────────────────────────────

TEST(RoleHandlerProcessor, SingleHub_TwoPresences_DedupToOneConnection)
{
    // Single-hub processor: `in_hub == out_hub`.  Two presences
    // (consumer on in_channel, producer on out_channel) collapse to
    // ONE HubConnection because (broker_endpoint, broker_pubkey) is
    // identical.  This is the operator-invisible optimisation per
    // role_host_template_design.md §5.4.
    auto hub = make_hub("tcp://127.0.0.1:5570", "shared-pubkey");
    std::vector<Presence> presences;
    presences.push_back(make_presence("proc.in", RoleKind::Consumer, hub));
    presences.push_back(make_presence("proc.out", RoleKind::Producer, hub));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 2u);
    EXPECT_EQ(h.connection_count(), 1u)
        << "Single-hub processor (in==out) must dedup to exactly one "
           "HubConnection (design §5.4).";

    const auto *p_in = h.find_presence_for_channel("proc.in");
    const auto *p_out = h.find_presence_for_channel("proc.out");
    ASSERT_NE(p_in, nullptr);
    ASSERT_NE(p_out, nullptr);
    EXPECT_EQ(p_in->role_kind, RoleKind::Consumer);
    EXPECT_EQ(p_out->role_kind, RoleKind::Producer);

    // BOTH presences must point at the SAME HubConnection slot — that
    // is the load-bearing invariant for shared-DEALER emission in M4.
    EXPECT_EQ(p_in->connection, p_out->connection)
        << "Both presences on a single-hub processor must share the "
           "same HubConnection pointer; dedup did not collapse them.";
}

TEST(RoleHandlerProcessor, DualHub_TwoPresences_TwoConnections)
{
    // Dual-hub processor: in_hub != out_hub → two presences, two
    // connections.  This is the topology Wave-B M8 enables end-to-end;
    // M3 verifies the data shape is in place.
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("proc.in", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5570")));
    presences.push_back(
        make_presence("proc.out", RoleKind::Producer, make_hub("tcp://127.0.0.1:5571")));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 2u);
    EXPECT_EQ(h.connection_count(), 2u) << "Dual-hub processor must materialise two distinct "
                                           "HubConnections — one per broker endpoint.";

    const auto *p_in = h.find_presence_for_channel("proc.in");
    const auto *p_out = h.find_presence_for_channel("proc.out");
    ASSERT_NE(p_in, nullptr);
    ASSERT_NE(p_out, nullptr);
    EXPECT_NE(p_in->connection, p_out->connection)
        << "Dual-hub processor presences must point at distinct "
           "HubConnection slots.";
    EXPECT_EQ(p_in->connection->broker_endpoint, "tcp://127.0.0.1:5570");
    EXPECT_EQ(p_out->connection->broker_endpoint, "tcp://127.0.0.1:5571");
}

TEST(RoleHandlerProcessor, MixedResolvedSame_PubkeyDifferences_TwoConnections)
{
    // Same endpoint string but different pubkey → distinct identities
    // → distinct connections.  Pin the contract that dedup compares
    // BOTH halves of the tuple, not just the endpoint.
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("proc.in", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5570", "pubkey-A")));
    presences.push_back(make_presence("proc.out", RoleKind::Producer,
                                      make_hub("tcp://127.0.0.1:5570", "pubkey-B")));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.connection_count(), 2u)
        << "Same endpoint with different pubkey is NOT dedup-equal — "
           "presence identity is the full (endpoint, pubkey) tuple.";
}

// ── Lookup edge cases ───────────────────────────────────────────────────────

TEST(RoleHandlerLookup, UnknownChannel_ReturnsNullptr)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("real.channel", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.find_presence_for_channel("nope"), nullptr);
    EXPECT_EQ(h.find_presence_for_channel(""), nullptr);
}

TEST(RoleHandlerLookup, EmptyChannelInPresence_NotIndexed)
{
    // A presence with an empty channel name (caller error or pre-
    // assignment placeholder) MUST NOT be indexed under `""` — that
    // would shadow other code paths checking for "not found" via empty
    // strings.  The presence still appears in `presences()` though.
    std::vector<Presence> presences;
    Presence p;
    p.hub = make_hub("tcp://127.0.0.1:5570");
    p.channel = ""; // deliberately empty
    p.role_kind = RoleKind::Producer;
    presences.push_back(std::move(p));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 1u);
    EXPECT_EQ(h.connection_count(), 1u);
    EXPECT_EQ(h.find_presence_for_channel(""), nullptr)
        << "Empty-channel presences MUST NOT be indexed; "
           "find_presence_for_channel(\"\") returns nullptr.";
}

TEST(RoleHandlerLookup, PresenceCountForChannel_DistinctChannels_AllOne)
{
    // Three distinct channels on three presences (a hypothetical
    // N-input router topology from design §5.2).
    std::vector<Presence> presences;
    presences.push_back(make_presence("chA", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5570")));
    presences.push_back(make_presence("chB", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5571")));
    presences.push_back(make_presence("chC", RoleKind::Producer, make_hub("tcp://127.0.0.1:5572")));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 3u);
    EXPECT_EQ(h.connection_count(), 3u);
    EXPECT_EQ(h.presence_count_for_channel("chA"), 1u);
    EXPECT_EQ(h.presence_count_for_channel("chB"), 1u);
    EXPECT_EQ(h.presence_count_for_channel("chC"), 1u);
    EXPECT_EQ(h.presence_count_for_channel("chZ"), 0u);
}

// ── Presence-pointer stability ──────────────────────────────────────────────

TEST(RoleHandlerDuplicate, DuplicateChannelOnRole_LogsError_AndIndexOnlyFirst)
{
    // Defensive contract: the per-role "no two presences share a
    // channel name" invariant is the caller's responsibility.  When
    // violated, `RoleHandler::build_channel_index_` MUST log an
    // ERROR and leave the index pointing at the FIRST presence (the
    // second is unreachable via `find_presence_for_channel`).  Both
    // presences still appear in `presences()`.
    //
    // This test path triggers `LOGGER_ERROR` — relies on the
    // binary-wide LifecycleGuard installed by
    // `PLH_BINARY_LIFECYCLE_MODULES` at the top of this file.
    auto hub = make_hub("tcp://127.0.0.1:5570");

    std::vector<Presence> presences;
    presences.push_back(make_presence("dup.chan", RoleKind::Consumer, hub));
    presences.push_back(make_presence("dup.chan", RoleKind::Producer, hub));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 2u)
        << "Both duplicates appear in the presence vector — the index "
           "is the only structure that dedups.";
    EXPECT_EQ(h.presence_count_for_channel("dup.chan"), 2u)
        << "presence_count_for_channel scans the full vector, not the "
           "index — surfaces the duplicate count for caller diagnostics.";

    const auto *p = h.find_presence_for_channel("dup.chan");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->role_kind, RoleKind::Consumer)
        << "Index resolution must return the FIRST presence on the "
           "channel (declaration order wins).";
}

TEST(RoleHandlerDuplicate, DuplicateChannel_SameRoleKind_AlsoLogsError)
{
    // Stronger duplicate case: two presences with IDENTICAL
    // `(hub, channel, role_kind)` tuples — design §5.1 forbids
    // this by construction.  The current index dedups by channel
    // name alone (role_kind is not part of the index key), so the
    // same first-wins + LOGGER_ERROR behavior applies regardless
    // of whether the duplicate is same-kind or cross-kind.  This
    // test pins that contract; if a future refactor adds role_kind
    // to the index key (and would silently accept same-channel
    // different-kind), this test continues to verify the
    // same-channel-same-kind path is still rejected.
    auto hub = make_hub("tcp://127.0.0.1:5570");

    std::vector<Presence> presences;
    presences.push_back(make_presence("dup.chan", RoleKind::Producer, hub));
    presences.push_back(make_presence("dup.chan", RoleKind::Producer, hub));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 2u);
    EXPECT_EQ(h.presence_count_for_channel("dup.chan"), 2u);
    const auto *p = h.find_presence_for_channel("dup.chan");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->role_kind, RoleKind::Producer);

    // Dedup also collapses to a single connection (same hub).
    EXPECT_EQ(h.connection_count(), 1u);
}

TEST(RoleHandlerStability, PresenceConnectionPointersAreValidPostConstruction)
{
    // After construction, `presences()` returns refs into the
    // RoleHandler's owned vector.  The `Presence::connection` pointer
    // wired during construction must reference a slot in the same
    // RoleHandler's `connections_` — not a dangling temporary.
    //
    // (Renamed from the original "PresencePointersStableAfterMove" —
    // the test never moves a RoleHandler, and RoleHandler is in fact
    // move-deleted; the original name was misleading.)
    std::vector<Presence> presences;
    auto hub = make_hub("tcp://127.0.0.1:5570");
    presences.push_back(make_presence("proc.in", RoleKind::Consumer, hub));
    presences.push_back(make_presence("proc.out", RoleKind::Producer, hub));

    RoleHandler h(std::move(presences));

    const auto &live_presences = h.presences();
    const auto &live_connections = h.connections();
    ASSERT_EQ(live_presences.size(), 2u);
    ASSERT_EQ(live_connections.size(), 1u);

    // Both presences' connection pointer must reference the single
    // slot in `live_connections`.  Address equality verifies that the
    // pointer wiring is stable + correct.
    EXPECT_EQ(live_presences[0].connection, &live_connections[0]);
    EXPECT_EQ(live_presences[1].connection, &live_connections[0]);
}

// ── Edge cases ──────────────────────────────────────────────────────────────

TEST(RoleHandlerEdgeCases, EmptyPresenceList_CtorSucceeds_LookupReturnsNullptr)
{
    // Defensive contract: an empty presence vector is valid input
    // (no crash, no error log).  All accessors return well-defined
    // empty/null results.  M4 callers that build the presence list
    // from config can pass an empty vector when config parsing
    // produces no presences; the handler does not assume ≥ 1.
    RoleHandler h(std::vector<Presence>{});

    EXPECT_EQ(h.presence_count(), 0u);
    EXPECT_EQ(h.connection_count(), 0u);
    EXPECT_EQ(h.find_presence_for_channel("anything"), nullptr);
    EXPECT_EQ(h.find_presence_for_channel(""), nullptr);
    EXPECT_EQ(h.presence_count_for_channel("anything"), 0u);
    EXPECT_TRUE(h.presences().empty());
    EXPECT_TRUE(h.connections().empty());
}

TEST(RoleHandlerProcessor, ConnectionsVector_FirstWinsOrdering_AcrossThreePresences)
{
    // 3-presence topology with a repeat: P0=hubA, P1=hubB, P2=hubA.
    // Pins the load-bearing "first-wins" ordering documented at
    // `role_handler.hpp::connections()` — important for M4's Class-B
    // fall-through dispatch where the first connection in the vector
    // is the primary target.  Mutation: change the dedup loop to
    // "last-wins" or shuffle the connections vector → this test fails.
    auto hubA = make_hub("tcp://127.0.0.1:5570", "pubkey-A");
    auto hubB = make_hub("tcp://127.0.0.1:5571", "pubkey-B");

    std::vector<Presence> presences;
    presences.push_back(make_presence("ch.first", RoleKind::Consumer, hubA));
    presences.push_back(make_presence("ch.second", RoleKind::Consumer, hubB));
    presences.push_back(make_presence("ch.third", RoleKind::Producer, hubA));

    RoleHandler h(std::move(presences));

    ASSERT_EQ(h.presence_count(), 3u);
    ASSERT_EQ(h.connection_count(), 2u) << "Two unique hubs across three presences must dedup to "
                                           "exactly two connections (hubA shared by P0 + P2).";

    // First-wins ordering: hubA was named first (P0), so it occupies
    // connections()[0]; hubB second.
    const auto &conns = h.connections();
    EXPECT_EQ(conns[0].broker_endpoint, "tcp://127.0.0.1:5570")
        << "First-named hub (hubA via P0) must occupy connections()[0].";
    EXPECT_EQ(conns[0].broker_pubkey, "pubkey-A");
    EXPECT_EQ(conns[1].broker_endpoint, "tcp://127.0.0.1:5571")
        << "Second-named hub (hubB via P1) must occupy connections()[1].";
    EXPECT_EQ(conns[1].broker_pubkey, "pubkey-B");

    // Presence-to-connection wiring: P0 + P2 share connections()[0]
    // (both on hubA); P1 alone on connections()[1].
    const auto &pres = h.presences();
    EXPECT_EQ(pres[0].connection, &conns[0]);
    EXPECT_EQ(pres[1].connection, &conns[1]);
    EXPECT_EQ(pres[2].connection, &conns[0])
        << "Third presence (hubA again) must bind to the existing "
           "hubA slot, not create a new one.";
}

// ── Wave-B M4b routing primitives ───────────────────────────────────────────
//
// brc_for_channel / brc_for_role / brc_for_band route requests to the
// right BrokerRequestComm.  on_band_joined / on_band_left populate the
// band index.  find_presence_from_notification routes inbound messages
// to their originating presence.
//
// These L2 tests verify the ROUTING LOGIC — the indexes are built
// correctly, lookups resolve to the right presences, edge cases return
// nullptr cleanly.  All `brc_for_*` tests run WITHOUT
// `start_connections()`, so the BRC pointer is always nullptr — the
// test asserts presence-routing via the slot identity, not via the
// (always-null) BRC.  L3 tests (Pattern 3) verify the post-start_
// `brc_for_*` returns non-null pointers that match `connections()[i].brc`.

TEST(RoleHandlerRouting, BrcForChannel_UnknownChannel_ReturnsNullptr)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("real.channel", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.brc_for_channel("unknown.channel"), nullptr);
    EXPECT_EQ(h.brc_for_channel(""), nullptr);
}

TEST(RoleHandlerRouting, BrcForChannel_BeforeStart_ReturnsNullptr_ButPresenceFound)
{
    // Pre-start_connections, BRC is unallocated → brc_for_channel
    // returns nullptr.  But find_presence_for_channel returns the
    // matching Presence (proves the channel IS in the index; only
    // the BRC is missing).  This is the disambiguation pattern
    // documented in the brc_for_channel docstring.
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("real.channel", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    EXPECT_FALSE(h.connections_started());
    EXPECT_NE(h.find_presence_for_channel("real.channel"), nullptr)
        << "Channel IS in the index — routing should resolve.";
    EXPECT_EQ(h.brc_for_channel("real.channel"), nullptr)
        << "BRC is unallocated pre-start — brc_for_channel returns "
           "nullptr until start_connections() runs.";
}

TEST(RoleHandlerRouting, BrcForRole_NoConnections_ReturnsNullptr)
{
    RoleHandler h(std::vector<Presence>{});
    EXPECT_EQ(h.brc_for_role(), nullptr)
        << "Zero-presence handler has no connections → no role-bound "
           "routing target.";
}

TEST(RoleHandlerRouting, BrcForRole_BeforeStart_ReturnsNullptr)
{
    std::vector<Presence> presences;
    presences.push_back(make_presence("ch", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.connection_count(), 1u);
    EXPECT_EQ(h.brc_for_role(), nullptr) << "Pre-start, the first connection's BRC is nullptr.";
}

TEST(RoleHandlerRouting, BrcForBand_NotJoined_ReturnsNullptr)
{
    std::vector<Presence> presences;
    presences.push_back(make_presence("ch", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.brc_for_band("not.joined.band"), nullptr);
    EXPECT_EQ(h.brc_for_band(""), nullptr);
}

TEST(RoleHandlerRouting, OnBandJoined_PopulatesIndex_FindByNotification)
{
    std::vector<Presence> presences;
    presences.push_back(make_presence("ch", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    const Presence *p = h.find_presence_for_channel("ch");
    ASSERT_NE(p, nullptr);

    // Before joining: band lookup returns nothing.
    EXPECT_EQ(h.brc_for_band("my.band"), nullptr);

    // Record join.
    h.on_band_joined("my.band", p);

    // BAND_*_NOTIFY body carries the band identifier under the
    // `band` key per HEP-CORE-0030 §5.1.  Pre-audit-B2 (2026-05-17)
    // this test synthesized `body["band_name"]` — an invented field
    // name that no broker ever emits — so it passed against the
    // matching invention in `find_presence_from_notification`
    // without exercising the real wire shape.
    nlohmann::json body;
    body["band"] = "my.band";
    EXPECT_EQ(h.find_presence_from_notification("BAND_BROADCAST_DELIVER_NOTIFY", body), p)
        << "After on_band_joined, find_presence_from_notification "
           "resolves body['band'] to the joined presence.";
}

TEST(RoleHandlerRouting, OnBandJoined_Idempotent_SamePair)
{
    std::vector<Presence> presences;
    presences.push_back(make_presence("ch", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    const Presence *p = h.find_presence_for_channel("ch");
    ASSERT_NE(p, nullptr);

    h.on_band_joined("my.band", p);
    h.on_band_joined("my.band", p); // idempotent

    nlohmann::json body;
    body["band"] = "my.band";
    EXPECT_EQ(h.find_presence_from_notification("X", body), p);
}

TEST(RoleHandlerRouting, OnBandJoined_OverwriteDifferentPresence_LastWins)
{
    // Two presences on different hubs; same band joined via each
    // sequentially — the second call overwrites the first per the
    // docstring contract.
    std::vector<Presence> presences;
    presences.push_back(make_presence("ch1", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5570")));
    presences.push_back(make_presence("ch2", RoleKind::Producer, make_hub("tcp://127.0.0.1:5571")));
    RoleHandler h(std::move(presences));

    const Presence *p0 = h.find_presence_for_channel("ch1");
    const Presence *p1 = h.find_presence_for_channel("ch2");
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);

    h.on_band_joined("shared.band", p0);
    h.on_band_joined("shared.band", p1);

    nlohmann::json body;
    body["band"] = "shared.band";
    EXPECT_EQ(h.find_presence_from_notification("X", body), p1)
        << "Last-wins: second on_band_joined overrides the band-to-"
           "presence mapping.";
}

TEST(RoleHandlerRouting, OnBandLeft_RemovesIndex)
{
    std::vector<Presence> presences;
    presences.push_back(make_presence("ch", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    const Presence *p = h.find_presence_for_channel("ch");
    ASSERT_NE(p, nullptr);

    h.on_band_joined("my.band", p);
    h.on_band_left("my.band");

    nlohmann::json body;
    body["band"] = "my.band";
    EXPECT_EQ(h.find_presence_from_notification("X", body), nullptr)
        << "Post-on_band_left, the band is no longer in the index.";

    // Idempotent: calling on_band_left on an absent band is safe.
    h.on_band_left("not.in.index");
}

TEST(RoleHandlerRouting, FindPresenceFromNotification_ChannelName_Routes)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("ch.a", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5570")));
    presences.push_back(
        make_presence("ch.b", RoleKind::Producer, make_hub("tcp://127.0.0.1:5571")));
    RoleHandler h(std::move(presences));

    nlohmann::json body;
    body["channel_name"] = "ch.b";
    const Presence *p = h.find_presence_from_notification("CHANNEL_CLOSING_NOTIFY", body);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->channel, "ch.b");
}

TEST(RoleHandlerRouting, FindPresenceFromNotification_UnknownChannel_Nullptr)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("ch.a", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    nlohmann::json body;
    body["channel_name"] = "ch.not.in.role";
    EXPECT_EQ(h.find_presence_from_notification("X", body), nullptr);
}

TEST(RoleHandlerRouting, FindPresenceFromNotification_NoChannelNoBand_Nullptr)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("ch.a", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    // Role-scope notification example: ROLE_DEREGISTERED_NOTIFY
    // typically has role_uid + reason, NOT channel_name or band.
    nlohmann::json body;
    body["role_uid"] = "prod.someone.uid";
    body["reason"] = "voluntary_close";
    EXPECT_EQ(h.find_presence_from_notification("ROLE_DEREGISTERED_NOTIFY", body), nullptr)
        << "No channel_name or band → caller routes via other "
           "logic (role-scope notifications don't bind to a "
           "specific presence).";
}

TEST(RoleHandlerRouting, FindPresenceFromNotification_NotObject_Nullptr)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("ch.a", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    nlohmann::json body_array = nlohmann::json::array();
    nlohmann::json body_string = "just a string";
    nlohmann::json body_null;

    EXPECT_EQ(h.find_presence_from_notification("X", body_array), nullptr);
    EXPECT_EQ(h.find_presence_from_notification("X", body_string), nullptr);
    EXPECT_EQ(h.find_presence_from_notification("X", body_null), nullptr);
}

TEST(RoleHandlerRouting, FindPresenceFromNotification_ChannelTakesPrecedenceOverBand)
{
    // When a notification body carries BOTH channel_name AND band
    // (rare but possible if a relay forwards a band event through a
    // channel context), channel routing wins.  Docstring says:
    // "Inspects body['channel_name'] FIRST (Class A — HEP-0007),
    // then body['band'] (Class D — HEP-0030 §5.1)."
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("ch.priority", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    presences.push_back(
        make_presence("ch.other", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5571")));
    RoleHandler h(std::move(presences));

    const Presence *p_ch = h.find_presence_for_channel("ch.priority");
    const Presence *p_band = h.find_presence_for_channel("ch.other");
    ASSERT_NE(p_ch, nullptr);
    ASSERT_NE(p_band, nullptr);
    h.on_band_joined("the.band", p_band);

    nlohmann::json body;
    body["channel_name"] = "ch.priority";
    body["band"] = "the.band";

    EXPECT_EQ(h.find_presence_from_notification("X", body), p_ch)
        << "Both fields present → channel wins per docstring "
           "precedence rule.";
}

TEST(RoleHandlerRouting, OnBandJoined_NullPresence_NoOp)
{
    // Defensive: passing nullptr presence is a caller bug; the
    // method silently skips rather than crashing.
    RoleHandler h(std::vector<Presence>{});
    h.on_band_joined("my.band", nullptr);

    nlohmann::json body;
    body["band"] = "my.band";
    EXPECT_EQ(h.find_presence_from_notification("X", body), nullptr);
}

TEST(RoleHandlerRouting, OnBandJoined_EmptyBandName_NoOp)
{
    std::vector<Presence> presences;
    presences.push_back(make_presence("ch", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));
    RoleHandler h(std::move(presences));

    const Presence *p = h.find_presence_for_channel("ch");
    ASSERT_NE(p, nullptr);

    // Empty band name is a caller bug; silently skipped.
    h.on_band_joined("", p);

    nlohmann::json body;
    body["band"] = "";
    EXPECT_EQ(h.find_presence_from_notification("X", body), nullptr);
}

// ── RoleHandler start/stop lifecycle ────────────────────────────────────
//
// Re-layered here from `test_layer3_datahub/test_datahub_role_state_machine.cpp`
// by the AUTH-6 layer-fit audit (2026-06-27).  See file header for the
// audit's correction reasoning.  The original L3 placement used a real
// `HubHost` broker purely as scaffolding for the endpoint string + a
// peer to attempt to connect to; no broker protocol behavior was
// asserted.

TEST(RoleHandlerLifecycle, StartStop_Smoke_SinglePresence)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("smoke.ch", RoleKind::Producer, make_curve_hub("tcp://127.0.0.1:5570")));
    RoleHandler handler(std::move(presences));

    EXPECT_EQ(handler.presence_count(), 1u);
    EXPECT_EQ(handler.connection_count(), 1u);
    EXPECT_FALSE(handler.connections_started());
    EXPECT_EQ(handler.connections()[0].brc.get(), nullptr);

    pylabhub::scripting::RoleHostCore core;
    pylabhub::scripting::RoleAPIBase api(core, "prod", "prod.smoke.uid00000001");
    api.set_name("smoke");

    // Post-start: brc allocated + connect() returned true.
    // `is_connected()` reads the BRC's internal `connected` flag set
    // at the end of `connect()` — no real broker required.
    ASSERT_TRUE(handler.start_connections(api));
    EXPECT_TRUE(handler.connections_started());
    ASSERT_NE(handler.connections()[0].brc.get(), nullptr);
    EXPECT_TRUE(handler.connections()[0].brc->is_connected());

    // Post-stop: brc released, flag clears.
    handler.stop_connections();
    EXPECT_FALSE(handler.connections_started());
    EXPECT_EQ(handler.connections()[0].brc.get(), nullptr);

    // Idempotency contract: second stop is a no-op.
    handler.stop_connections();
    EXPECT_FALSE(handler.connections_started());
}

TEST(RoleHandlerLifecycle, StartStop_DualHub_BothConnectionsConnected)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("dual.in", RoleKind::Consumer, make_curve_hub("tcp://127.0.0.1:5571")));
    presences.push_back(
        make_presence("dual.out", RoleKind::Producer, make_curve_hub("tcp://127.0.0.1:5572")));
    RoleHandler handler(std::move(presences));
    ASSERT_EQ(handler.connection_count(), 2u);

    pylabhub::scripting::RoleHostCore core;
    pylabhub::scripting::RoleAPIBase api(core, "proc", "proc.dual.uid00000001");
    api.set_name("dual_hub");

    ASSERT_TRUE(handler.start_connections(api));
    EXPECT_TRUE(handler.connections_started());
    EXPECT_TRUE(handler.connections()[0].brc->is_connected());
    EXPECT_TRUE(handler.connections()[1].brc->is_connected());

    handler.stop_connections();
    EXPECT_FALSE(handler.connections_started());
    EXPECT_EQ(handler.connections()[0].brc.get(), nullptr);
    EXPECT_EQ(handler.connections()[1].brc.get(), nullptr);
}

TEST(RoleHandlerLifecycle, DoubleStart_Rejected_StateNotCleared)
{
    std::vector<Presence> presences;
    presences.push_back(
        make_presence("double.ch", RoleKind::Producer, make_curve_hub("tcp://127.0.0.1:5573")));
    RoleHandler handler(std::move(presences));

    pylabhub::scripting::RoleHostCore core;
    pylabhub::scripting::RoleAPIBase api(core, "prod", "prod.double.uid00000001");
    api.set_name("double_start");

    ASSERT_TRUE(handler.start_connections(api));
    EXPECT_TRUE(handler.connections_started());

    EXPECT_FALSE(handler.start_connections(api))
        << "Double-start without intervening stop must be rejected.";
    EXPECT_TRUE(handler.connections_started())
        << "Rejected second start must NOT clear the started state.";

    handler.stop_connections();
}

TEST(RoleHandlerRouting, BrcForX_PostStart_PointerIdentity)
{
    // The pre-start nullptr branches are covered by
    // `BrcForChannel_BeforeStart_ReturnsNullptr_ButPresenceFound` and
    // `BrcForRole_BeforeStart_ReturnsNullptr`.  This test pins the
    // POST-start pointer identity for the 3 routing classes.
    const std::string ch = "routing.ch";
    std::vector<Presence> presences;
    presences.push_back(
        make_presence(ch, RoleKind::Producer, make_curve_hub("tcp://127.0.0.1:5574")));
    RoleHandler handler(std::move(presences));

    pylabhub::scripting::RoleHostCore core;
    pylabhub::scripting::RoleAPIBase api(core, "prod", "prod.routing.uid00000001");
    api.set_name("brc_routing");

    ASSERT_TRUE(handler.start_connections(api));

    auto *expected_brc = handler.connections()[0].brc.get();
    ASSERT_NE(expected_brc, nullptr);

    EXPECT_EQ(handler.brc_for_channel(ch), expected_brc)
        << "Class A routing: brc_for_channel must match the connection "
           "slot's BRC.";
    EXPECT_EQ(handler.brc_for_role(), expected_brc)
        << "Class B routing: brc_for_role returns the first "
           "connection's BRC (single-presence role).";

    // Class D: band routing requires `on_band_joined` first.
    EXPECT_EQ(handler.brc_for_band("test.band"), nullptr);
    const auto *p = handler.find_presence_for_channel(ch);
    ASSERT_NE(p, nullptr);
    handler.on_band_joined("test.band", p);
    EXPECT_EQ(handler.brc_for_band("test.band"), expected_brc)
        << "Class D routing: brc_for_band returns the BRC of the "
           "presence that joined the band.";

    handler.stop_connections();
}
