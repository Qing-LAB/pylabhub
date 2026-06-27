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

#include <zmq.h>  // zmq_z85_decode — decode setup keys for byte comparison

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
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

namespace
{

// Decode a Z85-encoded 40-char string to 32 raw bytes.  Used by the
// byte-equivalence assertions below (HEP-CORE-0040 §8.5.2 — the
// fixture's setup carries Z85; the KeyStore's `with_seckey` callback
// receives raw bytes; we compare at the raw boundary).
[[nodiscard]] std::array<std::byte, 32>
z85_decode_32(std::string_view z85)
{
    std::array<std::byte, 32> raw{};
    [[maybe_unused]] auto *rc = ::zmq_z85_decode(
        reinterpret_cast<uint8_t *>(raw.data()), z85.data());
    EXPECT_NE(rc, nullptr) << "zmq_z85_decode failed for input length "
                           << z85.size();
    return raw;
}

} // namespace

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

            const auto expected = z85_decode_32(setup.hub.secret_z85);

            bool seckey_matched = false;
            key_store().with_seckey(
                kHubIdentityName,
                [&](std::span<const std::byte> seckey)
                {
                    ASSERT_EQ(seckey.size(), expected.size())
                        << "seckey span size != 32 raw bytes (HEP-0040 "
                           "§8.5.2 contract violation)";
                    seckey_matched = (std::memcmp(seckey.data(),
                                                   expected.data(),
                                                   expected.size()) == 0);
                });

            EXPECT_TRUE(seckey_matched)
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
            const auto expected = z85_decode_32(setup.role(uid).secret_z85);
            const auto hub_bytes = z85_decode_32(setup.hub.secret_z85);

            // Defensive sanity: this assertion catches a degenerate
            // case where setup.role(uid) == setup.hub (which would
            // make the test trivially pass).
            ASSERT_NE(std::memcmp(expected.data(), hub_bytes.data(),
                                  expected.size()), 0)
                << "Sanity: setup.role(uid) == setup.hub — make_curve_setup "
                   "minted identical keypairs; test is degenerate.";

            const std::string name = pylabhub::tests::role_keystore_name(uid);
            bool seckey_matched = false;
            key_store().with_seckey(
                name,
                [&](std::span<const std::byte> seckey)
                {
                    seckey_matched = (std::memcmp(seckey.data(),
                                                   expected.data(),
                                                   expected.size()) == 0);
                });

            EXPECT_TRUE(seckey_matched)
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
