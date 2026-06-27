/**
 * @file test_curve_test_fixtures.cpp
 * @brief L2 protocol-validation tests for `CurveKeyStoreFixture`
 *        (HEP-CORE-0040 §172 + HEP-CORE-0035 §4.6.5 + task #177).
 *
 * The fixture lives in `tests/test_framework/curve_test_setup.h` and is
 * consumed by every L3 broker test worker that constructs `HubHost` or
 * `BrokerService` (HEP-CORE-0040 §172 mandates KeyStore-seeded hub
 * identity).  The fixture is THE PROTOCOL — the bypass-discipline
 * sanctioned by HEP-CORE-0035 §4.6.5 lives entirely in this wrapper
 * (skip on-disk vault + Argon2id; keep real CURVE on the wire).
 *
 * These tests pin the fixture's contract directly so future refactors
 * that silently change the protocol surface (a typo in a literal name,
 * a swap of pubkey↔seckey, an extra seeded entry) are caught at L2 before
 * the much-more-expensive L3 broker tests run.
 *
 * Each TEST_F runs in a fresh Pattern-3 subprocess because
 * `SecureMemorySubsystem` and `KeyStore` (the fixture's members) are
 * process singletons per HEP-CORE-0040 §4.1 + §5.1.
 *
 * What this file does NOT test (covered elsewhere):
 *   - `KeyStore` framework primitives — covered by `test_key_store.cpp`.
 *   - `BrokerService` ctor's KeyStore["hub_identity"] check — covered by
 *     `test_hub_state.cpp::BrokerServiceCtor::MissingHubIdentityInKeyStoreThrowsLogicError`.
 *   - End-to-end vault → CURVE wire — covered by L4 plh_hub_test /
 *     plh_role_test.
 *   - `start_hubhost_broker` production-parity — separate L3 test
 *     (deferred; the L4 path already proves vault → broker is correct,
 *     and L2 here proves fixture → KeyStore matches what vault would
 *     have produced; the L3 step is transitive).
 */

#include "test_patterns.h"

#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

namespace
{

class CurveKeyStoreFixtureTest : public IsolatedProcessTest
{
};

// ─── §1: canonical-name pins (mutation-protect literal names) ────────────────

TEST_F(CurveKeyStoreFixtureTest, Fixture_SeedsHubIdentity_UnderCanonicalName)
{
    // Mutation pinned: the fixture MUST seed under `kHubIdentityName`
    // (the literal name BrokerService ctor looks up).  A typo in the
    // fixture's seed_from() would leave the KeyStore populated but
    // under a wrong name, and tests would pass with a false sense of
    // CURVE coverage.
    auto w = SpawnWorker("curve_fixtures.seeds_hub_identity_under_canonical_name");
    ExpectWorkerOk(w);
}

TEST_F(CurveKeyStoreFixtureTest, Fixture_SeedsRoleIdentities_UnderRoleKeystoreName)
{
    // Mutation pinned: per-role entries MUST be at `role_keystore_name(uid)`
    // (i.e. "role." + uid) — the name BRC clients pass via
    // BrcHandle::start(keystore_name).  A divergence between the
    // fixture's seed convention and BRC's lookup convention would
    // cause a "KeyStore entry not found" failure deep in the BRC
    // poll loop instead of at fixture construction time.
    auto w = SpawnWorker("curve_fixtures.seeds_role_identities_under_role_keystore_name");
    ExpectWorkerOk(w);
}

// ─── §2: seed correctness (bytes match the setup) ────────────────────────────

TEST_F(CurveKeyStoreFixtureTest, Fixture_SeededBytes_MatchSetupForHubIdentity)
{
    // Mutation pinned: the fixture must hand setup.hub.public_z85 and
    // setup.hub.secret_z85 to KeyStore — NOT pubkey-and-pubkey, not
    // pubkey-and-empty, not seckey-and-pubkey.  This test invokes
    // `with_seckey` and verifies the callback receives the byte
    // sequence that decodes to setup.hub.secret_z85.
    auto w = SpawnWorker("curve_fixtures.seeded_bytes_match_setup_for_hub_identity");
    ExpectWorkerOk(w);
}

TEST_F(CurveKeyStoreFixtureTest, Fixture_SeededBytes_MatchSetupForRoleIdentity)
{
    // Mutation pinned: per-role bytes must come from setup.role_keys[uid],
    // NOT from setup.hub (a copy-paste in the seed loop would silently
    // give every role the hub's secret key).
    auto w = SpawnWorker("curve_fixtures.seeded_bytes_match_setup_for_role_identity");
    ExpectWorkerOk(w);
}

// ─── §3: minimality (no extra entries seeded) ────────────────────────────────

TEST_F(CurveKeyStoreFixtureTest, Fixture_SeedsExactlyTheDeclaredEntries)
{
    // Mutation pinned: the fixture's seeded entry count == 1 (hub) +
    // |role_keys|.  A future "convenience" addition that silently
    // seeds an extra entry (e.g. "broker_identity" as an alias) would
    // be caught here.  Per HEP-CORE-0040 §8.6, every seeded name MUST
    // be one the production code actually looks up.
    auto w = SpawnWorker("curve_fixtures.seeds_exactly_the_declared_entries");
    ExpectWorkerOk(w);
}

} // namespace
