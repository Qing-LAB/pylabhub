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

TEST(PubkeyOriginIndex, EmptyIndexResolvesNothingAndDeniesAll)
{
    const sec::PubkeyOriginIndex idx;

    EXPECT_TRUE(idx.empty());
    EXPECT_EQ(idx.size(), 0u);
    EXPECT_FALSE(idx.resolve(key('a')).has_value());

    // An empty index is the deny-all bootstrap (HEP-CORE-0035 §4.8.4),
    // NOT an admit-everyone state.
    const auto al = idx.as_peer_allowlist();
    EXPECT_TRUE(al.is_deny_all());
    EXPECT_FALSE(al.unrestricted);
}

TEST(PubkeyOriginIndex, LocalRoleResolvesToItsSubjectAndKind)
{
    sec::PubkeyOriginIndex idx;
    idx.add_local_role(make_role("prod.sensor.uid01", key('a'), "temperature sensor"));

    const auto origin = idx.resolve(key('a'));
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(origin->kind, sec::PubkeyOrigin::Kind::LocalRole);
    EXPECT_EQ(origin->subject_uid, "prod.sensor.uid01");
    EXPECT_EQ(origin->subject_name, "temperature sensor");
}

TEST(PubkeyOriginIndex, FederationPeerResolvesToPeerKind)
{
    sec::PubkeyOriginIndex idx;
    idx.add_federation_peer("hub.west", key('b'), "west wing hub");

    const auto origin = idx.resolve(key('b'));
    ASSERT_TRUE(origin.has_value());
    // The kind is load-bearing: only a FederationPeer may carry
    // identities other than its own.  A role mis-typed as a peer would
    // silently gain that right.
    EXPECT_EQ(origin->kind, sec::PubkeyOrigin::Kind::FederationPeer);
    EXPECT_EQ(origin->subject_uid, "hub.west");
}

TEST(PubkeyOriginIndex, UnknownKeyResolvesToNothing)
{
    sec::PubkeyOriginIndex idx;
    idx.add_local_role(make_role("prod.sensor.uid01", key('a')));

    EXPECT_FALSE(idx.resolve(key('z')).has_value());
    // A near-miss must not resolve either — no prefix or partial match.
    EXPECT_FALSE(idx.resolve(std::string(39, 'a')).has_value());
    EXPECT_FALSE(idx.resolve("").has_value());
}

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
    const auto origin = idx.resolve(key('a'));
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(origin->subject_uid, "prod.sensor.uid01");
    EXPECT_EQ(idx.size(), 1u);
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
