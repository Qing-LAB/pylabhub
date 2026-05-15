/**
 * @file test_role_handler.cpp
 * @brief L2 unit tests for `RoleHandler` — Wave-B M3 skeleton.
 *
 * Pure data-structure tests: presence enumeration, dedup correctness,
 * channel index lookup, lifetime invariants on the pointer wiring,
 * and the duplicate-channel defensive log.
 *
 * Pattern selection: **Pattern 1+** (`BinaryLifecycleEnvironment` via
 * `PLH_BINARY_LIFECYCLE_MODULES`).  `RoleHandler::build_channel_index_`
 * emits `LOGGER_ERROR` on a duplicate-channel input — Logger is a
 * lifecycle-backed module per HEP-CORE-0001, so any test of code that
 * can emit `LOGGER_*` needs the binary-wide guard.  See
 * `docs/README/README_testing.md § "Pattern 1+"` for the rationale +
 * the decision checklist (Q1: "Does my test call any `LOGGER_*` macro?
 * (Logger is lifecycle-backed.)").
 *
 * Compiled as its own executable `test_layer2_role_handler` — NOT
 * part of any aggregate target — because `BinaryLifecycleEnvironment`
 * requires being the only `LifecycleGuard` owner in its binary.
 *
 * Wave-B M4 will add network-touching tests covering `start()` /
 * `shutdown()` / dispatch — those go to L3 (Pattern 3
 * IsolatedProcessTest) because they spawn ThreadManager threads
 * and connect real DEALER sockets to a `BrokerService`.
 */

#include "binary_lifecycle.h"
#include "utils/logger.hpp"
#include "utils/role_handler.hpp"

#include <gtest/gtest.h>

// Binary-wide LifecycleGuard for Logger.  Required because
// `RoleHandler::build_channel_index_` emits `LOGGER_ERROR` on a
// duplicate-channel input.  This is the only `LifecycleGuard` owner
// in this binary by design — see file header.
PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule()
)

#include <string>
#include <vector>

using pylabhub::config::HubRefConfig;
using pylabhub::scripting::HubConnection;
using pylabhub::scripting::Presence;
using pylabhub::scripting::RoleHandler;
using pylabhub::scripting::RoleKind;
using pylabhub::scripting::parse_wire_string;
using pylabhub::scripting::to_wire_string;

namespace
{

HubRefConfig make_hub(const std::string &endpoint,
                      const std::string &pubkey = "test-pubkey-aaaa")
{
    HubRefConfig h;
    h.broker        = endpoint;
    h.broker_pubkey = pubkey;
    return h;
}

Presence make_presence(const std::string &channel,
                       RoleKind           kind,
                       HubRefConfig       hub)
{
    Presence p;
    p.hub       = std::move(hub);
    p.channel   = channel;
    p.role_kind = kind;
    return p;
}

}  // namespace

// ── RoleKind wire-string round-trip ─────────────────────────────────────────

TEST(RoleKindWire, ToString)
{
    EXPECT_STREQ(to_wire_string(RoleKind::Producer), "producer");
    EXPECT_STREQ(to_wire_string(RoleKind::Consumer), "consumer");
}

TEST(RoleKindWire, ParseRoundTrip)
{
    RoleKind k;
    ASSERT_TRUE(parse_wire_string("producer", k));
    EXPECT_EQ(k, RoleKind::Producer);
    ASSERT_TRUE(parse_wire_string("consumer", k));
    EXPECT_EQ(k, RoleKind::Consumer);
}

TEST(RoleKindWire, ParseRejectsUnknownAndEmpty)
{
    RoleKind k = RoleKind::Producer;  // sentinel
    EXPECT_FALSE(parse_wire_string("", k));
    EXPECT_FALSE(parse_wire_string("processor", k));  // not a wire role_type
    EXPECT_FALSE(parse_wire_string("PRODUCER", k));   // case-sensitive
    EXPECT_EQ(k, RoleKind::Producer)
        << "k must be left untouched on parse failure";
}

// ── Single-presence (producer/consumer) topologies ──────────────────────────

TEST(RoleHandlerSinglePresence, Producer_OneConnection)
{
    std::vector<Presence> presences;
    presences.push_back(make_presence(
        "test.out", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));

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
    presences.push_back(make_presence(
        "test.in", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5570")));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 1u);
    EXPECT_EQ(h.connection_count(), 1u);
    ASSERT_NE(h.find_presence_for_channel("test.in"), nullptr);
    EXPECT_EQ(h.find_presence_for_channel("test.in")->role_kind,
              RoleKind::Consumer);
}

// ── Processor topologies — the dedup payoff ────────────────────────────────

TEST(RoleHandlerProcessor, SingleHub_TwoPresences_DedupToOneConnection)
{
    // Single-hub processor: `in_hub == out_hub`.  Two presences
    // (consumer on in_channel, producer on out_channel) collapse to
    // ONE HubConnection because (broker_endpoint, broker_pubkey) is
    // identical.  This is the operator-invisible optimisation per
    // role_host_template_design.md §5.4.
    auto                  hub = make_hub("tcp://127.0.0.1:5570", "shared-pubkey");
    std::vector<Presence> presences;
    presences.push_back(make_presence("proc.in",  RoleKind::Consumer, hub));
    presences.push_back(make_presence("proc.out", RoleKind::Producer, hub));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 2u);
    EXPECT_EQ(h.connection_count(), 1u)
        << "Single-hub processor (in==out) must dedup to exactly one "
           "HubConnection (design §5.4).";

    const auto *p_in  = h.find_presence_for_channel("proc.in");
    const auto *p_out = h.find_presence_for_channel("proc.out");
    ASSERT_NE(p_in,  nullptr);
    ASSERT_NE(p_out, nullptr);
    EXPECT_EQ(p_in->role_kind,  RoleKind::Consumer);
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
    presences.push_back(make_presence(
        "proc.in",  RoleKind::Consumer, make_hub("tcp://127.0.0.1:5570")));
    presences.push_back(make_presence(
        "proc.out", RoleKind::Producer, make_hub("tcp://127.0.0.1:5571")));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(), 2u);
    EXPECT_EQ(h.connection_count(), 2u)
        << "Dual-hub processor must materialise two distinct "
           "HubConnections — one per broker endpoint.";

    const auto *p_in  = h.find_presence_for_channel("proc.in");
    const auto *p_out = h.find_presence_for_channel("proc.out");
    ASSERT_NE(p_in,  nullptr);
    ASSERT_NE(p_out, nullptr);
    EXPECT_NE(p_in->connection, p_out->connection)
        << "Dual-hub processor presences must point at distinct "
           "HubConnection slots.";
    EXPECT_EQ(p_in->connection->broker_endpoint,  "tcp://127.0.0.1:5570");
    EXPECT_EQ(p_out->connection->broker_endpoint, "tcp://127.0.0.1:5571");
}

TEST(RoleHandlerProcessor, MixedResolvedSame_PubkeyDifferences_TwoConnections)
{
    // Same endpoint string but different pubkey → distinct identities
    // → distinct connections.  Pin the contract that dedup compares
    // BOTH halves of the tuple, not just the endpoint.
    std::vector<Presence> presences;
    presences.push_back(make_presence(
        "proc.in",  RoleKind::Consumer,
        make_hub("tcp://127.0.0.1:5570", "pubkey-A")));
    presences.push_back(make_presence(
        "proc.out", RoleKind::Producer,
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
    presences.push_back(make_presence(
        "real.channel", RoleKind::Producer, make_hub("tcp://127.0.0.1:5570")));

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
    Presence              p;
    p.hub       = make_hub("tcp://127.0.0.1:5570");
    p.channel   = "";  // deliberately empty
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
    presences.push_back(make_presence(
        "chA", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5570")));
    presences.push_back(make_presence(
        "chB", RoleKind::Consumer, make_hub("tcp://127.0.0.1:5571")));
    presences.push_back(make_presence(
        "chC", RoleKind::Producer, make_hub("tcp://127.0.0.1:5572")));

    RoleHandler h(std::move(presences));

    EXPECT_EQ(h.presence_count(),    3u);
    EXPECT_EQ(h.connection_count(),  3u);
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

TEST(RoleHandlerStability, PresencePointersStableAfterMove)
{
    // After construction, `presences()` returns refs into the
    // RoleHandler's owned vector.  The `Presence::connection` pointer
    // wired during construction must reference a slot in the same
    // RoleHandler's `connections_` — not a dangling temporary.
    std::vector<Presence> presences;
    auto                  hub = make_hub("tcp://127.0.0.1:5570");
    presences.push_back(make_presence("proc.in",  RoleKind::Consumer, hub));
    presences.push_back(make_presence("proc.out", RoleKind::Producer, hub));

    RoleHandler h(std::move(presences));

    const auto &live_presences   = h.presences();
    const auto &live_connections = h.connections();
    ASSERT_EQ(live_presences.size(),   2u);
    ASSERT_EQ(live_connections.size(), 1u);

    // Both presences' connection pointer must reference the single
    // slot in `live_connections`.  Address equality verifies that the
    // pointer wiring is stable + correct.
    EXPECT_EQ(live_presences[0].connection, &live_connections[0]);
    EXPECT_EQ(live_presences[1].connection, &live_connections[0]);
}
