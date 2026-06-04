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
#include <filesystem>
#include <memory>
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
    std::unique_ptr<pylabhub::hub::HubState>          hub_state;
    std::unique_ptr<pylabhub::broker::BrokerService>  service;
    std::thread                                        thread;
    std::string                                        endpoint;
    std::string                                        pubkey;

    DirectBrokerHandle()                                = default;
    DirectBrokerHandle(const DirectBrokerHandle &)      = delete;
    DirectBrokerHandle &operator=(const DirectBrokerHandle &) = delete;
    DirectBrokerHandle(DirectBrokerHandle &&) noexcept  = default;
    DirectBrokerHandle &operator=(DirectBrokerHandle &&) noexcept = default;
    ~DirectBrokerHandle();

    void stop_and_join();
};

/// Start a direct broker.  Wires CURVE + admission from `setup` and
/// applies the caller's `cfg` (timeouts, policies, etc.) on top.  The
/// returned handle is ready for use — `endpoint` and `pubkey` are
/// populated from the broker's on_ready callback before this returns.
[[nodiscard]] DirectBrokerHandle
start_direct_broker(pylabhub::broker::BrokerService::Config cfg,
                    const CurveSetup &setup);

// ─── HubHost broker — full production assembly ───────────────────────────────

/// Owns a temp `hub_dir`, the `HubConfig` loaded from it, and the
/// `HubHost` instance running the broker.  Move-only.  The dtor calls
/// `host->shutdown()` and removes the temp dir.
struct HubHostBrokerHandle
{
    std::filesystem::path                            hub_dir;
    std::unique_ptr<pylabhub::hub_host::HubHost>     host;
    std::string                                       endpoint;
    std::string                                       pubkey;

    HubHostBrokerHandle()                                   = default;
    HubHostBrokerHandle(const HubHostBrokerHandle &)        = delete;
    HubHostBrokerHandle &operator=(const HubHostBrokerHandle &) = delete;
    HubHostBrokerHandle(HubHostBrokerHandle &&) noexcept    = default;
    HubHostBrokerHandle &operator=(HubHostBrokerHandle &&) noexcept = default;
    ~HubHostBrokerHandle();

    void stop_and_join();
};

/// Start a HubHost broker with a freshly-initialized hub directory.
/// `j_overrides` is merged on top of the default `hub.json` template
/// emitted by `HubDirectory::init_directory` (for tests that want
/// non-default timeouts, admin disabled, etc.); pass an empty json
/// for defaults.  CURVE keypair is injected via
/// `HubConfig::inject_keypair_for_test` (HEP-CORE-0035 §4.6.5).
///
/// The hub_dir's `vault/known_roles.json` is written with the
/// allowlist from `setup` so the broker's Layer-1 ZAP gate admits
/// every role in `setup.role_keys`.
///
/// `hub_name` is passed to `HubDirectory::init_directory` (operator-
/// visible hub label); the uid is auto-derived.
[[nodiscard]] HubHostBrokerHandle
start_hubhost_broker(const nlohmann::json &j_overrides,
                     const CurveSetup &setup,
                     std::string_view hub_name = "HarnessTestHub");

// ─── BRC client — BrokerRequestComm + poll-loop thread ───────────────────────

/// Owns a `BrokerRequestComm` + the thread running its `run_poll_loop`.
/// Lifecycle:
///   - `start(...)`  binds the BRC config (broker endpoint + pubkey +
///                   client keypair + role_uid), connects, spawns the
///                   poll thread.
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
///   bh.start(ep, pk, uid, kp);
struct BrcHandle
{
    pylabhub::hub::BrokerRequestComm brc;
    std::atomic<bool>                 running{true};
    std::thread                       thread;

    BrcHandle()                              = default;
    BrcHandle(const BrcHandle &)             = delete;
    BrcHandle &operator=(const BrcHandle &)  = delete;
    BrcHandle(BrcHandle &&)                  = delete;
    BrcHandle &operator=(BrcHandle &&)       = delete;
    ~BrcHandle();

    void start(const std::string &endpoint, const std::string &server_pubkey,
               const std::string &role_uid, const CurveKeypair &role_kp);
    void stop();
};

} // namespace pylabhub::tests
