#pragma once
/**
 * @file broker_test_harness.h
 * @brief Shared L3 broker / BRC test harness.
 *
 * Before this file existed, each L3 worker translation unit re-declared
 * its own `BrokerHandle` / `BrcHandle` + `start_broker_*()` helper —
 * 15+ near-identical copies across `tests/test_layer3_datahub/workers/`.
 * Each variant carried slightly different ownership semantics, slightly
 * different teardown order, and inconsistent CURVE wiring.
 *
 * This header consolidates the three shapes that actually matter:
 *
 *   - `DirectBrokerHandle` — direct `BrokerService` + `HubState`, no
 *     `HubHost`.  Used by tests that exercise broker internals against
 *     hand-rolled `BrokerService::Config` (heartbeat overrides, etc.).
 *
 *   - `HubHostBrokerHandle` — the full production assembly: hub_dir +
 *     `HubConfig` + `HubHost` + broker thread.  Used by tests that
 *     exercise broker behavior through the real production callsite.
 *     Production-shaped: the broker comes up the same way `plh_hub run`
 *     brings it up, minus the vault (HEP-CORE-0035 §4.6.5 sanctions
 *     skipping the on-disk vault while keeping real CURVE on the wire).
 *
 *   - `BrcHandle` — `BrokerRequestComm` + poll-loop thread.  Used by
 *     every L3 test that drives the broker via the BRC client.
 *
 * All three accept the CURVE setup bundle from `curve_test_setup.h`
 * and wire CURVE + admission unconditionally per HEP-CORE-0035 §2 +
 * §4.6.5.  No test bypass mode exists.
 */

#include "curve_test_setup.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/json_fwd.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

// Forward decls — keep the header light.
namespace pylabhub::hub
{
class HubState;
}
namespace pylabhub::hub_host
{
class HubHost;
}

namespace pylabhub::tests
{

// ─── Direct broker — BrokerService + HubState, no HubHost ────────────────────

/// Owns a `HubState`, a `BrokerService` constructed against that state,
/// and the broker run thread.  Move-only.  The dtor is the canonical
/// teardown — explicit `stop_and_join()` is also available for tests
/// that want to control teardown timing.
struct DirectBrokerHandle
{
    std::unique_ptr<pylabhub::hub::HubState> hub_state;
    std::unique_ptr<pylabhub::broker::BrokerService> service;
    std::thread thread;
    std::string endpoint;
    std::string pubkey;

    DirectBrokerHandle() = default;
    DirectBrokerHandle(const DirectBrokerHandle &) = delete;
    DirectBrokerHandle &operator=(const DirectBrokerHandle &) = delete;
    DirectBrokerHandle(DirectBrokerHandle &&) noexcept = default;
    DirectBrokerHandle &operator=(DirectBrokerHandle &&) noexcept = default;
    ~DirectBrokerHandle();

    void stop_and_join();
};

/// Start a direct broker.  Wires CURVE + admission from `setup` and
/// applies the caller's `cfg` (timeouts, policies, etc.) on top.  The
/// returned handle is ready for use — `endpoint` and `pubkey` are
/// populated from the broker's on_ready callback before this returns.
[[nodiscard]] DirectBrokerHandle start_direct_broker(pylabhub::broker::BrokerService::Config cfg,
                                                     const CurveSetup &setup);

// ─── HubHost broker — full production assembly ───────────────────────────────

/// Owns a temp `hub_dir`, the `HubConfig` loaded from it, and the
/// `HubHost` instance running the broker.  Move-only.  The dtor calls
/// `host->shutdown()` and removes the temp dir.
struct HubHostBrokerHandle
{
    std::filesystem::path hub_dir;
    std::unique_ptr<pylabhub::hub_host::HubHost> host;
    std::string endpoint;
    std::string pubkey;

    HubHostBrokerHandle() = default;
    HubHostBrokerHandle(const HubHostBrokerHandle &) = delete;
    HubHostBrokerHandle &operator=(const HubHostBrokerHandle &) = delete;
    HubHostBrokerHandle(HubHostBrokerHandle &&) noexcept = default;
    HubHostBrokerHandle &operator=(HubHostBrokerHandle &&) noexcept = default;
    ~HubHostBrokerHandle();

    void stop_and_join();

    /// Convenience accessor for the HubHost's owned BrokerService.
    /// Tests that need broker-internal state (channel snapshot, admin
    /// queries, request-close-channel) reach it via this helper to
    /// avoid threading `broker.host->broker()` through every callsite.
    [[nodiscard]] pylabhub::broker::BrokerService &service();
};

/// Start a HubHost broker with a freshly-initialized hub directory.
///
/// CALLER CONTRACT (HEP-CORE-0040 §172 + HEP-CORE-0035 §4.8): the
/// caller seeds ONLY the per-role identities — a
/// `pylabhub::tests::seed_role_identities(setup)` (one `"role.<uid>"`
/// KeyStore entry per role in the bundle).  Callers MUST NOT call
/// `seed_curve_identities`: this helper OWNS `"hub_identity"` — it
/// provisions a REAL encrypted hub vault (`provision_hub_vault`,
/// Argon2id) and loads it through the PRODUCTION
/// `HubConfig::load_keypair` path (`load_hub_keypair_fresh`), so a
/// caller-side hub seed would double-seed and throw at the KeyStore.
///
/// ALLOWLIST: `setup.role_keys` is written INTO the encrypted vault's
/// `known_roles` document (HEP-CORE-0035 §4.8) during provisioning so
/// the broker's Layer-1 ZAP gate admits every role in the bundle.
/// There is NO plaintext `known_roles.json` sidecar — per §4.8.7 the
/// hub REFUSES to start while one exists.
///
/// PARAMETERS:
///   - `j_overrides` is merged on top of the default `hub.json`
///     template emitted by `HubDirectory::init_directory` (for
///     tests that want non-default timeouts, admin disabled, etc.).
///     Pass an empty json for defaults.
///   - `hub_name` is passed to `HubDirectory::init_directory`
///     (operator-visible hub label); the uid is auto-derived.
[[nodiscard]] HubHostBrokerHandle
start_hubhost_broker(const nlohmann::json &j_overrides, const CurveSetup &setup,
                     std::string_view hub_name = "HarnessTestHub");

// ─── BRC client — BrokerRequestComm + poll-loop thread ───────────────────────

/// Owns a `BrokerRequestComm` + the thread running its `run_poll_loop`.
/// Lifecycle:
///   - `start(...)`  binds the BRC config (broker endpoint + pubkey +
///                   role_uid + keystore_name), connects, spawns the
///                   poll thread.  HEP-CORE-0040 §172 use-not-export:
///                   the BRC fetches the role's seckey by NAME, never
///                   by bytes.
///   - `stop()`      flips the loop guard, stops the BRC's socket,
///                   joins the thread, disconnects.
///   - dtor         best-effort `stop()` if a start ran without a
///                  matching explicit stop.
///
/// Tests that need notification callbacks register them on `brc`
/// BEFORE calling `start()` (so the first dispatched notification
/// already sees the handler):
///
///   BrcHandle bh;
///   bh.brc.on_notification([](auto type, auto body){ ... });
///   bh.start(ep, pk, uid, pylabhub::tests::role_keystore_name(uid));
struct BrcHandle
{
    pylabhub::hub::BrokerRequestComm brc;
    std::atomic<bool> running{true};
    std::thread thread;

    BrcHandle() = default;
    BrcHandle(const BrcHandle &) = delete;
    BrcHandle &operator=(const BrcHandle &) = delete;
    BrcHandle(BrcHandle &&) = delete;
    BrcHandle &operator=(BrcHandle &&) = delete;
    ~BrcHandle();

    /// Connect a BRC to a HubHost broker.  Caller is responsible for
    /// having seeded the process KeyStore (typically via
    /// `seed_curve_identities()`) under the name `keystore_name` BEFORE
    /// calling.  `keystore_name` MUST be the same name used to seed
    /// the KeyStore — the canonical form is
    /// `pylabhub::tests::role_keystore_name(role_uid)`.
    ///
    /// HEP-CORE-0040 §172: the role keypair lives only in locked
    /// memory; this signature passes a NAME, not bytes.
    void start(const std::string &endpoint, const std::string &server_pubkey,
               const std::string &role_uid, const std::string &keystore_name);
    void stop();
};

// ── REG_REQ / CONSUMER_REG_REQ payload builders ─────────────────────────────
//
// Per HEP-CORE-0036 §5b + §6.1 + §I10: REG_REQ MUST carry `zmq_pubkey`, and
// there is EXACTLY ONE pubkey per role uid (KnownRolesStore::add() enforces
// the injective mapping at load time).  The pubkey is therefore functionally
// derivable from the role_uid — the helper looks it up from the process
// `secure().keys()` via the canonical `role.<uid>` entry name
// (`pylabhub::tests::role_keystore_name`).  Callers MUST have seeded the
// keystore via `seed_curve_identities()` before calling.
//
// Pre-2026-06-29 the 5 L3 worker files each carried a private copy with
// divergent signatures (3-param "caller-supplies-pubkey" vs 2-param
// "helper-looks-it-up").  The 3-param shape was older — written when callers
// had a `CurveSetup` in scope; the 2-param shape is the design-consistent
// post-strict-CURVE form (KeyStore is the canonical identity store per
// HEP-CORE-0040 §172).  Consolidated under this single 2-param shape
// (REVIEW_C2 F10).
//
// `producer_pid` / `consumer_pid` default to `::getpid()` — the common case.
// Tests that need to pin a specific pid (e.g. dereg-pid-mismatch assertions
// in broker_consumer_workers.cpp) pass it explicitly.
[[nodiscard]] nlohmann::json make_reg_opts(const std::string &channel, const std::string &role_uid,
                                           std::optional<uint64_t> producer_pid = std::nullopt,
                                           const std::string &channel_topology = {});

[[nodiscard]] nlohmann::json make_cons_opts(const std::string &channel,
                                            const std::string &consumer_uid,
                                            std::optional<uint64_t> consumer_pid = std::nullopt,
                                            const std::string &channel_topology = {});

// Negative-path overload — caller supplies the wire `zmq_pubkey`
// explicitly so the test can inject a value that DOES NOT match the
// keystore entry for `role_uid` (or use a `role_uid` that has no
// keystore entry at all).  Used by tests that pin the broker's
// UNKNOWN_ROLE / PUBKEY_MISMATCH rejection paths
// (HEP-CORE-0036 §6.3 Layer-2 verify_known_role_binding).
//
// Happy-path tests should use the keystore-derived overload above —
// it can't accidentally diverge from the broker's known_roles record.
[[nodiscard]] nlohmann::json
make_reg_opts_with_explicit_pubkey(const std::string &channel, const std::string &role_uid,
                                   const std::string &zmq_pubkey,
                                   std::optional<uint64_t> producer_pid = std::nullopt);

} // namespace pylabhub::tests
