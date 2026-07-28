/**
 * @file test_pubkey_origin.cpp
 * @brief L2 pins for the pubkey origin index (HEP-CORE-0035 §4.2).
 *
 * The index is the single structure that answers "what does this CURVE
 * key mean to this hub."  Every identity decision above the socket
 * resolves through it, so these tests pin the contract the design
 * states, not merely the behaviour the current code happens to have:
 *
 *   - a key resolves to its subject, and to the right KIND (a local
 *     role and a federation peer must never be confused — the kind
 *     decides whether the subject may carry identities other than its
 *     own, HEP-CORE-0035 §4.3);
 *   - an unknown key resolves to nothing, never to a default;
 *   - one key configured for two subjects is REFUSED, because a
 *     resolvable-either-way key would make every gate above it depend
 *     on config ordering;
 *   - the ZAP allowlist projection is derived from the index and is
 *     never unrestricted;
 *   - the inbox roster exposes local roles ONLY — handing a federation
 *     peer's key to the role-to-role plane would authorize the wrong
 *     plane (HEP-CORE-0027 §3.5).
 */
#include "utils/role_identity_policy.hpp"
#include "utils/security/pubkey_origin.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace
{

namespace sec = pylabhub::utils::security;

/// Distinct, well-formed 40-char Z85 keys.  Content is irrelevant to
/// the index (it never does crypto); the LENGTH is contractual.
std::string key(char fill)
{
    return std::string(40, fill);
}

pylabhub::broker::KnownRole make_role(std::string uid, std::string pubkey, std::string name = {})
{
    pylabhub::broker::KnownRole r;
    r.uid = std::move(uid);
    r.pubkey_z85 = std::move(pubkey);
    r.name = std::move(name);
    return r;
}

// ─── Where the resolution tests went ────────────────────────────────────────
//
// `resolve()` takes an `AttestedKey`, which only `AttestedKey::from_transport`
// can produce and only where a ZAP domain is actually enforced.  So the
// resolution cases need a live ZAP domain and live in
// `ZapRouterTest.Index_ResolvesAttestedKeys` (worker
// `zap_router::index_resolves_attested_keys`).
//
// Two former cases are GONE rather than moved: resolving a 39-character key
// and resolving an empty string.  Neither is expressible any more —
// `Z85PublicKey::validate` refuses both before an `AttestedKey` can exist —
// so they were tests of a hole the type now closes.  The cases below are the
// ones that remain genuinely about the INDEX: what it refuses to store, and
// what its projections contain.

TEST(PubkeyOriginIndex, OneKeyForTwoRolesIsRefused)
{
    sec::PubkeyOriginIndex idx;
    idx.add_local_role(make_role("prod.sensor.uid01", key('a')));

    // Two subjects behind one key cannot be resolved to one identity.
    // Refusing is the only safe answer; picking a winner would make the
    // resolved identity depend on config order.
    EXPECT_THROW(idx.add_local_role(make_role("prod.sensor.uid02", key('a'))),
                 std::runtime_error);

    // The original mapping survives the rejected insert unchanged —
    // a refused write must not corrupt the index.
}

TEST(PubkeyOriginIndex, KeySharedBetweenRoleAndPeerIsRefused)
{
    sec::PubkeyOriginIndex idx;
    idx.add_local_role(make_role("prod.sensor.uid01", key('a')));

    // This is the collision the known-roles store cannot see, because
    // roles and federation peers are configured separately.  The index
    // is the first place both categories meet, so it is where the
    // cross-category clash must be caught.
    EXPECT_THROW(idx.add_federation_peer("hub.west", key('a')), std::runtime_error);
    EXPECT_EQ(idx.size(), 1u);
}

TEST(PubkeyOriginIndex, ReAddingTheSameSubjectAndKeyIsIdempotent)
{
    sec::PubkeyOriginIndex idx;
    idx.add_local_role(make_role("prod.sensor.uid01", key('a'), "sensor"));
    // Same subject, same key — a reload of identical config, not a clash.
    EXPECT_NO_THROW(idx.add_local_role(make_role("prod.sensor.uid01", key('a'), "sensor")));
    EXPECT_EQ(idx.size(), 1u);
}

TEST(PubkeyOriginIndex, MalformedKeyLengthIsRefused)
{
    sec::PubkeyOriginIndex idx;

    EXPECT_THROW(idx.add_local_role(make_role("prod.sensor.uid01", "")), std::runtime_error);
    EXPECT_THROW(idx.add_local_role(make_role("prod.sensor.uid01", std::string(39, 'a'))),
                 std::runtime_error);
    EXPECT_THROW(idx.add_local_role(make_role("prod.sensor.uid01", std::string(41, 'a'))),
                 std::runtime_error);
    EXPECT_THROW(idx.add_federation_peer("hub.west", "short"), std::runtime_error);
    EXPECT_TRUE(idx.empty());
}

TEST(PubkeyOriginIndex, AllowlistProjectsEveryKeyOfBothKinds)
{
    sec::PubkeyOriginIndex idx;
    idx.add_local_role(make_role("prod.sensor.uid01", key('a')));
    idx.add_local_role(make_role("cons.logger.uid02", key('b')));
    idx.add_federation_peer("hub.west", key('c'));

    const auto al = idx.as_peer_allowlist();
    // The control-plane allowlist admits roles AND peer hubs — both
    // legitimately connect to the broker's control socket.
    EXPECT_EQ(al.peers.size(), 3u);
    EXPECT_TRUE(al.contains(sec::PeerIdentity{"curve", key('a')}));
    EXPECT_TRUE(al.contains(sec::PeerIdentity{"curve", key('b')}));
    EXPECT_TRUE(al.contains(sec::PeerIdentity{"curve", key('c')}));
    EXPECT_FALSE(al.contains(sec::PeerIdentity{"curve", key('z')}));
    EXPECT_FALSE(al.unrestricted);
}

TEST(PubkeyOriginIndex, InboxRosterExcludesFederationPeers)
{
    sec::PubkeyOriginIndex idx;
    idx.add_local_role(make_role("prod.sensor.uid01", key('b')));
    idx.add_local_role(make_role("cons.logger.uid02", key('a')));
    idx.add_federation_peer("hub.west", key('c'));

    const auto roster = idx.local_role_pubkeys();
    // Role-to-role messaging authorizes local roles only; a peer hub's
    // key belongs to a different plane and must not leak into it.
    ASSERT_EQ(roster.size(), 2u);
    EXPECT_EQ(roster[0], key('a'));
    EXPECT_EQ(roster[1], key('b')); // sorted — stable across processes
}

} // namespace
