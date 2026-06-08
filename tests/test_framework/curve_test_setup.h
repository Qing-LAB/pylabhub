#pragma once
/**
 * @file curve_test_setup.h
 * @brief Real-CURVE setup helper for tests (HEP-CORE-0035 §4.6.5 +
 *        HEP-CORE-0040 §172).
 *
 * HEP-CORE-0035 §2 + §4.6.5 codify a no-bypass discipline: tests
 * exercise the same authenticated wire path production does.  No
 * test runs against a CURVE-disabled or admission-disabled broker.
 * HEP-CORE-0040 §172 adds the use-not-export contract: role/hub
 * identity keypairs live ONLY in the process `KeyStore` (locked
 * memory); test fixtures must construct `SecureMemorySubsystem` +
 * `KeyStore` and seed them with identities BEFORE the BRC / queue
 * code paths run.
 *
 * Typical fixture wiring:
 *
 * @code
 * // Mint hub + per-role keypairs (no vault file — keys live only in
 * // the test process's heap until handed to the fixture).
 * auto setup = pylabhub::tests::make_curve_setup({uid_a, uid_b});
 *
 * // RAII: constructs SecureMemorySubsystem + KeyStore (one per
 * // process) and seeds "hub_identity" + "role.<uid>" for each role.
 * pylabhub::tests::CurveKeyStoreFixture ks_fixture(
 *     "test", "test.fixture", setup);
 *
 * // BrokerService::Config carries no keypair (HEP-CORE-0040 §172) —
 * // apply_curve_to only seeds known_roles[].  The CURVE keypair is
 * // read from `key_store()` by name at run() time.
 * BrokerService::Config bs_cfg;
 * pylabhub::tests::apply_curve_to(bs_cfg, setup);
 *
 * // BRC for a specific role identity:
 * BrokerRequestComm::Config brc_cfg;
 * brc_cfg.broker_pubkey   = setup.hub.public_z85;
 * brc_cfg.keystore_name   = pylabhub::tests::role_keystore_name(uid_a);
 * @endcode
 *
 * CURVE + ZAP install in `BrokerService::run()` is unconditional
 * (HEP-CORE-0035 §2 + §4.6.5).  `BrokerService` ctor throws if
 * `KeyStore` entry `"hub_identity"` is absent, so every fixture must
 * construct a `CurveKeyStoreFixture` before building the broker.
 */

#include "utils/broker_service.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/known_roles.hpp"   // pylabhub::broker::KnownRole
#include "utils/security/secure_memory_subsystem.hpp"

#include <zmq.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pylabhub::tests
{

/// Z85-encoded CURVE keypair (each field is 40 ASCII chars).
struct CurveKeypair
{
    std::string public_z85;
    std::string secret_z85;
};

/// Generate a fresh CURVE keypair via libzmq's wrapper around
/// libsodium.  Cost: ~100 μs.  Returns empty strings + an
/// `ADD_FAILURE` if libzmq fails (should not happen in practice).
inline CurveKeypair gen_curve_keypair()
{
    char pub[41]{};
    char sec[41]{};
    const int rc = ::zmq_curve_keypair(pub, sec);
    if (rc != 0)
    {
        ADD_FAILURE() << "zmq_curve_keypair failed with rc=" << rc;
        return {};
    }
    return CurveKeypair{std::string(pub, 40), std::string(sec, 40)};
}

/// Build a `KnownRole` allowlist entry pinning a role identity to
/// a CURVE pubkey.  Default role-name / role-side are reasonable
/// for the common producer-side fixture; override at the call site
/// when the test exercises a consumer-only or "any"-side scenario.
inline pylabhub::broker::KnownRole
make_known_role(const std::string &role_uid,
                const std::string &pubkey_z85,
                const std::string &role_name = "test.role",
                const std::string &role      = "producer")
{
    pylabhub::broker::KnownRole kr;
    kr.name       = role_name;
    kr.uid        = role_uid;
    kr.role       = role;
    kr.pubkey_z85 = pubkey_z85;
    return kr;
}

/// Bundled CURVE state for a single broker fixture: the hub's
/// server keypair plus one client keypair per role uid the test
/// will register.  `make_curve_setup({uid_a, uid_b, ...})` is the
/// usual one-liner; the returned struct is passed to
/// `apply_curve_to(cfg, setup)` to wire the broker config in one
/// call, and the per-role keys are pulled out via `setup.role(uid)`
/// for the BRC client construction.
struct CurveSetup
{
    CurveKeypair                       hub;
    std::map<std::string, CurveKeypair> role_keys;

    /// Lookup helper with an explicit assertion so a test that
    /// forgot to mint a role's keypair fails loudly instead of
    /// returning an empty keypair.
    [[nodiscard]] const CurveKeypair &role(const std::string &uid) const
    {
        auto it = role_keys.find(uid);
        if (it == role_keys.end())
        {
            ADD_FAILURE() << "CurveSetup::role(\"" << uid
                          << "\") — uid was not declared at make_curve_setup() time";
            static const CurveKeypair empty{};
            return empty;
        }
        return it->second;
    }
};

/// Construct a CURVE bundle for a list of role uids.  Each uid
/// gets a fresh keypair.  Tests that need additional uids later
/// can mint them ad-hoc via `gen_curve_keypair()` and push another
/// `make_known_role` into the broker cfg before startup.
inline CurveSetup make_curve_setup(const std::vector<std::string> &role_uids)
{
    CurveSetup s;
    s.hub = gen_curve_keypair();
    for (const auto &uid : role_uids)
    {
        s.role_keys.emplace(uid, gen_curve_keypair());
    }
    return s;
}

/// Wire a `BrokerService::Config` for admission using the bundle
/// from `make_curve_setup`.  Pushes a `KnownRole` for every uid in
/// `setup.role_keys` onto `cfg.known_roles[]` so the broker admits
/// each role's connection.
///
/// HEP-CORE-0040 §172: the hub's CURVE keypair is NO LONGER carried
/// on `BrokerService::Config` — `BrokerService::run()` reads it from
/// `key_store()` under `"hub_identity"`.  Callers MUST construct a
/// `CurveKeyStoreFixture` (which seeds `"hub_identity"` from
/// `setup.hub`) BEFORE building the `BrokerService` instance.
inline void apply_curve_to(pylabhub::broker::BrokerService::Config &cfg,
                           const CurveSetup &setup)
{
    for (const auto &[uid, kp] : setup.role_keys)
    {
        cfg.known_roles.push_back(make_known_role(uid, kp.public_z85));
    }
}

/// Canonical KeyStore name for a role uid in tests.  Production
/// code uses the singular `"role_identity"` (one keypair per role
/// process); test fixtures with multiple BRCs in one process pick
/// per-uid names under this `"role." + uid` convention.  Use it
/// both for `BRC::Config::keystore_name` and `ZmqAuthOptions::keystore_name`.
[[nodiscard]] inline std::string
role_keystore_name(const std::string &uid)
{
    return "role." + uid;
}

/// RAII fixture: construct `SecureMemorySubsystem` + `KeyStore` and
/// seed identities from a `CurveSetup`.
///
/// Per HEP-CORE-0040 §4.5 + §5.1 there must be at most one
/// SecureMemorySubsystem and one KeyStore per process — instantiate
/// this fixture exactly once per Pattern-3 subprocess (or once per
/// L2 test binary, depending on test layering).  Constructing it
/// twice in the same process throws from the KeyStore ctor.
///
/// Loads:
///   - `"hub_identity"`          ← `setup.hub`
///   - `"role." + uid`           ← `setup.role_keys.at(uid)` for each entry
///
/// Tests that need an additional ad-hoc identity (minted after
/// fixture construction) call `add_identity(name, kp)` directly.
class CurveKeyStoreFixture
{
public:
    /// @param scope_tag  KeyStore scope tag ("test" or "test.<sub>");
    ///                   non-empty.  No behavioral effect — just goes
    ///                   into the lifecycle-module name.
    /// @param owner_id   KeyStore owner uid; non-empty.
    /// @param setup      Identities to seed into the new KeyStore.
    CurveKeyStoreFixture(const std::string &scope_tag,
                         const std::string &owner_id,
                         const CurveSetup  &setup)
        : sms_(),
          ks_(scope_tag, owner_id)
    {
        seed_from(setup);
    }

    CurveKeyStoreFixture(const CurveKeyStoreFixture &)            = delete;
    CurveKeyStoreFixture &operator=(const CurveKeyStoreFixture &) = delete;
    CurveKeyStoreFixture(CurveKeyStoreFixture &&)                 = delete;
    CurveKeyStoreFixture &operator=(CurveKeyStoreFixture &&)      = delete;

    /// Add one identity under `name` from a CurveKeypair.  Delegates
    /// to `KeyStore::add_identity_from_z85` — the single production
    /// site where the (pub_z85 || sec_z85) → 80-byte layout is
    /// defined.  No parallel packing logic lives in this fixture.
    static void
    add_identity(std::string_view name, const CurveKeypair &kp)
    {
        pylabhub::utils::security::key_store().add_identity_from_z85(
            name, kp.public_z85, kp.secret_z85);
    }

private:
    pylabhub::utils::security::SecureMemorySubsystem sms_;
    pylabhub::utils::security::KeyStore              ks_;

    void seed_from(const CurveSetup &setup)
    {
        add_identity(pylabhub::utils::security::kHubIdentityName, setup.hub);
        for (const auto &[uid, kp] : setup.role_keys)
        {
            add_identity(role_keystore_name(uid), kp);
        }
    }
};

} // namespace pylabhub::tests
