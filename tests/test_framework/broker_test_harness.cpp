/**
 * @file broker_test_harness.cpp
 * @brief Implementation of the shared L3 broker test harness.
 *
 * See `broker_test_harness.h` for the design rationale.  All wiring
 * here uses real CURVE + admission per HEP-CORE-0035 §2 + §4.6.5 —
 * there is no bypass switch.
 */
#include "broker_test_harness.h"

#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/security/known_roles.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace pylabhub::tests
{

// ─── DirectBrokerHandle ──────────────────────────────────────────────────────

DirectBrokerHandle::~DirectBrokerHandle()
{
    if (thread.joinable())
    {
        if (service)
            service->stop();
        thread.join();
    }
}

void DirectBrokerHandle::stop_and_join()
{
    if (service)
        service->stop();
    if (thread.joinable())
        thread.join();
}

DirectBrokerHandle
start_direct_broker(pylabhub::broker::BrokerService::Config cfg,
                    const CurveSetup &setup)
{
    // Wire CURVE + admission from `setup` on top of the caller's
    // policy fields.  Per HEP-CORE-0035 §2 both are unconditional;
    // BrokerService ctor enforces non-empty server keys.
    cfg.server_public_key      = setup.hub.public_z85;
    cfg.server_secret_key      = setup.hub.secret_z85;
    for (const auto &[uid, kp] : setup.role_keys)
        cfg.known_roles.push_back(make_known_role(uid, kp.public_z85));

    using Ready = std::pair<std::string, std::string>;
    auto promise = std::make_shared<std::promise<Ready>>();
    auto fut     = promise->get_future();
    cfg.on_ready = [promise](const std::string &ep, const std::string &pk) {
        promise->set_value({ep, pk});
    };

    auto state = std::make_unique<pylabhub::hub::HubState>();
    auto svc   = std::make_unique<pylabhub::broker::BrokerService>(
        std::move(cfg), *state);
    auto raw = svc.get();
    std::thread t([raw]() { raw->run(); });
    auto info = fut.get();

    DirectBrokerHandle h;
    h.hub_state = std::move(state);
    h.service   = std::move(svc);
    h.thread    = std::move(t);
    h.endpoint  = std::move(info.first);
    h.pubkey    = std::move(info.second);
    return h;
}

// ─── HubHostBrokerHandle ─────────────────────────────────────────────────────

HubHostBrokerHandle::~HubHostBrokerHandle()
{
    stop_and_join();
}

void HubHostBrokerHandle::stop_and_join()
{
    if (host)
    {
        host->shutdown();
        host.reset();
    }
    if (!hub_dir.empty())
    {
        std::error_code ec;
        fs::remove_all(hub_dir, ec);
        hub_dir.clear();
    }
}

pylabhub::broker::BrokerService &HubHostBrokerHandle::service()
{
    return host->broker();
}

namespace
{
fs::path make_unique_hub_dir(std::string_view hub_name_for_log)
{
    (void) hub_name_for_log;
    static std::atomic<int> ctr{0};
    fs::path dir = fs::temp_directory_path() /
                   ("plh_l3_harness_" + std::to_string(::getpid()) + "_" +
                    std::to_string(ctr.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}
} // anon

HubHostBrokerHandle
start_hubhost_broker(const nlohmann::json &j_overrides,
                     const CurveSetup &setup,
                     std::string_view hub_name)
{
    // MASKED — pending HEP-0035 §4.6.5 step 3-revised-F migration (#154).
    //
    // The previous body used `HubConfig::inject_keypair_for_test` to
    // skip the on-disk vault while keeping real CURVE on the wire.
    // That product surface was deleted in #151 (test-shaped API in
    // product code violates HEP-CORE-0035 §4.6.5 no-bypass discipline).
    //
    // The replacement path is to write a real vault via
    // `HubConfig::create_keypair(password)` + decrypt via
    // `HubConfig::load_keypair(password)` — slow (Argon2id) but
    // identical to production.  That migration is part of #154,
    // which re-creates every L3 broker fixture against the
    // refactored lib code.  Until then this helper stays a stub so
    // the framework library compiles; its callers (the L3 worker
    // bodies in `tests/test_layer3_datahub/workers/*.cpp`) are
    // excluded from the build via the #153 CMakeLists masks.
    (void)j_overrides;
    (void)setup;
    (void)hub_name;
    throw std::logic_error(
        "broker_test_harness::start_hubhost_broker — MASKED pending "
        "#154 (HEP-CORE-0035 §4.6.5 step 3-revised-F migration).  The "
        "vault-on-disk replacement for inject_keypair_for_test lands "
        "with the L3 broker test re-creation.");
}

// ─── BrcHandle ───────────────────────────────────────────────────────────────

BrcHandle::~BrcHandle()
{
    if (thread.joinable())
        stop();
}

void BrcHandle::start(const std::string &endpoint,
                      const std::string &server_pubkey,
                      const std::string &role_uid,
                      const CurveKeypair &role_kp)
{
    pylabhub::hub::BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = endpoint;
    cfg.broker_pubkey   = server_pubkey;
    cfg.client_pubkey   = role_kp.public_z85;
    cfg.client_seckey   = role_kp.secret_z85;
    cfg.role_uid        = role_uid;
    ASSERT_TRUE(brc.connect(cfg));
    thread = std::thread([this] {
        brc.run_poll_loop([this] { return running.load(); });
    });
}

void BrcHandle::stop()
{
    running.store(false);
    brc.stop();
    if (thread.joinable())
        thread.join();
    brc.disconnect();
}

} // namespace pylabhub::tests
