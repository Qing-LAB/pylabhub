#pragma once
/**
 * @file curve_test_setup.h
 * @brief Real-CURVE setup helper for tests (HEP-CORE-0035 §4.6.5).
 *
 * HEP-CORE-0035 §2 + §4.6.5 codify a no-bypass discipline: tests
 * exercise the same authenticated wire path production does.  No
 * test runs against a CURVE-disabled or admission-disabled broker.
 * This header is the shared minimal toolkit fixtures use to wire
 * real CURVE keypairs + a populated `known_roles[]` allowlist
 * without going through the vault layer (vault persistence is a
 * runtime concern; in-memory keys are just two Z85 strings).
 *
 * Cost per fixture: ~100 μs per keypair generation (libsodium
 * underneath `zmq_curve_keypair`).  No Argon2id, no password, no
 * vault file — keys live only in the test process's heap.
 *
 * Typical fixture wiring:
 *
 * @code
 * auto hub_kp  = pylabhub::tests::gen_curve_keypair();
 * auto role_kp = pylabhub::tests::gen_curve_keypair();
 *
 * BrokerService::Config cfg;
 * cfg.use_curve              = true;
 * cfg.enforce_ctrl_admission = true;
 * cfg.server_public_key      = hub_kp.public_z85;
 * cfg.server_secret_key      = hub_kp.secret_z85;
 * cfg.known_roles.push_back(pylabhub::tests::make_known_role(
 *     "prod.test.uid1", role_kp.public_z85));
 *
 * // BRC client uses the role keypair to connect:
 * brc_cfg.broker_pubkey = hub_kp.public_z85;
 * brc_cfg.client_pubkey = role_kp.public_z85;
 * brc_cfg.client_seckey = role_kp.secret_z85;
 * @endcode
 *
 * The `cfg.use_curve = true; cfg.enforce_ctrl_admission = true;`
 * lines are transitional — both fields are slated for deletion in
 * the HEP-0035 landing phase (after which `BrokerService` installs
 * CURVE + ZAP unconditionally and these two assignments disappear
 * from every fixture).  Until then, set them explicitly to make the
 * fixture's no-bypass posture grep-visible.
 */

#include "utils/broker_service.hpp"
#include "utils/role_identity_policy.hpp"   // KnownRole

#include <zmq.h>

#include <gtest/gtest.h>

#include <map>
#include <string>
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

/// Wire a `BrokerService::Config` for CURVE + admission using the
/// bundle from `make_curve_setup`.  Pushes a `KnownRole` for every
/// uid in `setup.role_keys` onto `cfg.known_roles[]` so the broker
/// admits each role's connection.
inline void apply_curve_to(pylabhub::broker::BrokerService::Config &cfg,
                           const CurveSetup &setup)
{
    cfg.use_curve              = true;
    cfg.enforce_ctrl_admission = true;
    cfg.server_public_key      = setup.hub.public_z85;
    cfg.server_secret_key      = setup.hub.secret_z85;
    for (const auto &[uid, kp] : setup.role_keys)
    {
        cfg.known_roles.push_back(make_known_role(uid, kp.public_z85));
    }
}

} // namespace pylabhub::tests
