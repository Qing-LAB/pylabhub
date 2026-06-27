/**
 * @file curve_test_fixtures_workers.cpp
 * @brief Worker bodies for `test_curve_test_fixtures.cpp` (HEP-CORE-0040
 *        §172 + HEP-CORE-0035 §4.6.5 + task #177).
 *
 * Each worker constructs a `CurveKeyStoreFixture` and pins one
 * facet of the fixture's seed contract.  Worker subprocess required
 * because `SecureMemorySubsystem` + `KeyStore` are process singletons
 * (HEP-CORE-0040 §4.1 + §5.1).
 */
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_memory_subsystem.hpp"

#include "curve_test_setup.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;
using pylabhub::utils::security::key_store;
using pylabhub::utils::security::kHubIdentityName;

namespace pylabhub::tests::worker
{
namespace curve_fixtures
{

// ─── §1: canonical-name pins ─────────────────────────────────────────────────

int seeds_hub_identity_under_canonical_name()
{
    return run_gtest_worker(
        [] {
            auto setup = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "test", "test.l2.curve_fixtures.canonical_hub_name",
                setup);

            // The literal contract: BrokerService ctor looks up
            // kHubIdentityName.  Fixture must seed under that exact
            // symbol — mutation safety against a divergent literal.
            ASSERT_TRUE(key_store().has(kHubIdentityName))
                << "Fixture failed to seed under kHubIdentityName "
                   "('" << kHubIdentityName << "').  A typo or rename "
                   "in CurveKeyStoreFixture::seed_from() would land "
                   "here before any L3 broker test surfaces it.";

            // And the pubkey must round-trip back to setup.hub.public_z85.
            EXPECT_EQ(key_store().pubkey(kHubIdentityName),
                      setup.hub.public_z85)
                << "Seeded pubkey under kHubIdentityName does not "
                   "match setup.hub.public_z85 — fixture handed the "
                   "wrong half-key or hashed it.";
        },
        "curve_fixtures::seeds_hub_identity_under_canonical_name",
        Logger::GetLifecycleModule());
}

int seeds_role_identities_under_role_keystore_name()
{
    return run_gtest_worker(
        [] {
            const std::string uid_a = "prod.test.curve_fixture.uid_a";
            const std::string uid_b = "cons.test.curve_fixture.uid_b";
            auto setup = pylabhub::tests::make_curve_setup({uid_a, uid_b});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "test", "test.l2.curve_fixtures.canonical_role_name",
                setup);

            // The literal contract: BRC client looks up
            // `role_keystore_name(uid)` ("role." + uid).  Fixture
            // must seed under that exact convention.
            const std::string name_a =
                pylabhub::tests::role_keystore_name(uid_a);
            const std::string name_b =
                pylabhub::tests::role_keystore_name(uid_b);

            ASSERT_TRUE(key_store().has(name_a))
                << "Fixture failed to seed role '" << uid_a
                << "' under role_keystore_name() ('" << name_a
                << "').  BRC clients would fail their KeyStore lookup "
                   "at start() time instead of at fixture construction.";
            ASSERT_TRUE(key_store().has(name_b))
                << "Fixture failed to seed role '" << uid_b
                << "' under role_keystore_name() ('" << name_b
                << "').  Same failure shape as above.";

            EXPECT_EQ(key_store().pubkey(name_a),
                      setup.role(uid_a).public_z85);
            EXPECT_EQ(key_store().pubkey(name_b),
                      setup.role(uid_b).public_z85);
        },
        "curve_fixtures::seeds_role_identities_under_role_keystore_name",
        Logger::GetLifecycleModule());
}

// ─── §2: seed correctness (bytes match the setup) ────────────────────────────

int seeded_bytes_match_setup_for_hub_identity()
{
    return run_gtest_worker(
        [] {
            auto setup = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "test", "test.l2.curve_fixtures.bytes_hub", setup);

            // Use the Z85 accessor for direct string compare — the
            // setup carries Z85 (40 ASCII chars per half-key) and
            // `with_seckey_z85` exposes the same encoding on demand
            // (encoded on-the-fly from raw bytes, sodium_memzero'd on
            // exit per HEP-CORE-0040 §8.5.2 wire/file/display rule).
            std::string seckey_seen;
            key_store().with_seckey_z85(
                kHubIdentityName,
                [&](std::string_view seckey_z85)
                {
                    ASSERT_EQ(seckey_z85.size(), 40u)
                        << "with_seckey_z85 yielded " << seckey_z85.size()
                        << " chars; HEP-0040 §8.5.2 specifies 40-char Z85.";
                    seckey_seen.assign(seckey_z85);
                });

            EXPECT_EQ(seckey_seen, setup.hub.secret_z85)
                << "Fixture seeded a seckey that does NOT match "
                   "setup.hub.secret_z85 — suggests pubkey↔seckey swap, "
                   "truncated copy, or wrong half-key handed to "
                   "KeyStore::add_identity_from_z85.";
        },
        "curve_fixtures::seeded_bytes_match_setup_for_hub_identity",
        Logger::GetLifecycleModule());
}

int seeded_bytes_match_setup_for_role_identity()
{
    return run_gtest_worker(
        [] {
            const std::string uid = "prod.test.curve_fixture.uid_bytes";
            auto setup = pylabhub::tests::make_curve_setup({uid});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "test", "test.l2.curve_fixtures.bytes_role", setup);

            // The role's seckey must come from setup.role_keys[uid],
            // NOT from setup.hub.  A copy-paste in the seed loop
            // (using setup.hub instead of kp) would silently give
            // every role the hub's secret key.
            const std::string expected = setup.role(uid).secret_z85;

            // Defensive sanity: catches the case where make_curve_setup
            // mints identical keypairs for the hub and the role
            // (probability 1/2^256 but cheap to assert; without it,
            // the test would trivially pass).
            ASSERT_NE(expected, setup.hub.secret_z85)
                << "Sanity: setup.role(uid).secret_z85 == "
                   "setup.hub.secret_z85 — make_curve_setup minted "
                   "identical keypairs; test is degenerate.";

            const std::string name = pylabhub::tests::role_keystore_name(uid);
            std::string seckey_seen;
            key_store().with_seckey_z85(
                name,
                [&](std::string_view seckey_z85)
                {
                    seckey_seen.assign(seckey_z85);
                });

            EXPECT_EQ(seckey_seen, expected)
                << "Fixture seeded role '" << uid << "' with bytes "
                   "that do NOT match setup.role(uid).secret_z85.";
        },
        "curve_fixtures::seeded_bytes_match_setup_for_role_identity",
        Logger::GetLifecycleModule());
}

// ─── §3: minimality (no extra entries seeded) ────────────────────────────────

int seeds_exactly_the_declared_entries()
{
    return run_gtest_worker(
        [] {
            const std::string uid_a = "prod.test.curve_fixture.uid_n1";
            const std::string uid_b = "cons.test.curve_fixture.uid_n2";
            auto setup = pylabhub::tests::make_curve_setup({uid_a, uid_b});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "test", "test.l2.curve_fixtures.minimality", setup);

            // Expected: 1 hub + |role_keys| role entries; no aliases,
            // no admin tokens, no diagnostic entries.  A fixture that
            // silently seeds extras would be caught here — and the
            // extras would also represent unused secret material in
            // the process's locked memory.
            const std::size_t expected = 1u + setup.role_keys.size();
            EXPECT_EQ(key_store().size(), expected)
                << "Fixture seeded " << key_store().size()
                << " entries; expected " << expected
                << " (1 hub + " << setup.role_keys.size()
                << " roles).  An extra silently-seeded entry would "
                   "represent unused locked-memory budget and a "
                   "divergence from the documented contract in "
                   "curve_test_setup.h:184-244.";

            // Spot-check the entries we know are correct exist.
            EXPECT_TRUE(key_store().has(kHubIdentityName));
            EXPECT_TRUE(key_store().has(
                pylabhub::tests::role_keystore_name(uid_a)));
            EXPECT_TRUE(key_store().has(
                pylabhub::tests::role_keystore_name(uid_b)));
        },
        "curve_fixtures::seeds_exactly_the_declared_entries",
        Logger::GetLifecycleModule());
}

} // namespace curve_fixtures
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ─────────────────────────────────────────────────────

namespace
{

struct CurveFixturesRegistrar
{
    CurveFixturesRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "curve_fixtures")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::curve_fixtures;

                if (sc == "seeds_hub_identity_under_canonical_name")
                    return seeds_hub_identity_under_canonical_name();
                if (sc == "seeds_role_identities_under_role_keystore_name")
                    return seeds_role_identities_under_role_keystore_name();
                if (sc == "seeded_bytes_match_setup_for_hub_identity")
                    return seeded_bytes_match_setup_for_hub_identity();
                if (sc == "seeded_bytes_match_setup_for_role_identity")
                    return seeded_bytes_match_setup_for_role_identity();
                if (sc == "seeds_exactly_the_declared_entries")
                    return seeds_exactly_the_declared_entries();
                return -1;
            });
    }
} g_registrar;

} // namespace
