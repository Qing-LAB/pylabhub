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
    // policy fields.  Per HEP-CORE-0035 §2, neither is optional.
    cfg.use_curve              = true;
    cfg.enforce_ctrl_admission = true;
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
    HubHostBrokerHandle h;
    h.hub_dir = make_unique_hub_dir(hub_name);

    // 1. Initialize the hub directory + default hub.json template.
    pylabhub::utils::HubDirectory::init_directory(
        h.hub_dir, std::string(hub_name));

    // 2. Merge overrides into hub.json.  Tests commonly want
    //    network.broker_endpoint = "tcp://127.0.0.1:0" (ephemeral
    //    port), admin.enabled = false, script.path = "".
    const fs::path hub_json_path = h.hub_dir / "hub.json";
    nlohmann::json j;
    {
        std::ifstream f(hub_json_path);
        if (f.is_open())
            j = nlohmann::json::parse(f);
    }
    j.merge_patch(j_overrides);
    {
        std::ofstream f(hub_json_path);
        f << j.dump(2);
    }

    // 3. Write the known_roles vault entry so the broker's Layer-1
    //    ZAP gate admits each role in `setup`.  HEP-CORE-0035 §4.8.2
    //    stores the allowlist inside the encrypted vault; here we
    //    bypass the vault (per §4.6.5) and write a plaintext
    //    known_roles.json — the format `KnownRolesStore::load_from_file`
    //    reads.  This is the SAME bypass as `inject_keypair_for_test`:
    //    skip persistence, keep the wire path identical to production.
    if (!setup.role_keys.empty())
    {
        const fs::path vault_subdir = h.hub_dir / "vault";
        fs::create_directories(vault_subdir);
        const fs::path known_roles_path = vault_subdir / "known_roles.json";
        nlohmann::json roles = nlohmann::json::array();
        for (const auto &[uid, kp] : setup.role_keys)
        {
            roles.push_back({
                {"name",       "test.role"},
                {"uid",        uid},
                {"role",       "producer"},
                {"pubkey_z85", kp.public_z85},
            });
        }
        nlohmann::json kr_doc;
        kr_doc["known_roles"] = roles;
        std::ofstream f(known_roles_path);
        f << kr_doc.dump(2);
        // ACL: KnownRolesStore::load_from_file expects 0600.
        std::error_code ec;
        fs::permissions(known_roles_path,
                        fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
        fs::permissions(vault_subdir,
                        fs::perms::owner_all,
                        fs::perm_options::replace, ec);
    }

    // 4. Load HubConfig, inject the CURVE keypair (HEP-CORE-0035 §4.6.5
    //    test bypass — vault skipped, real CURVE on the wire),
    //    construct HubHost, startup.
    auto hcfg = pylabhub::config::HubConfig::load_from_directory(
        h.hub_dir.string());
    hcfg.inject_keypair_for_test(setup.hub.public_z85, setup.hub.secret_z85);

    h.host = std::make_unique<pylabhub::hub_host::HubHost>(std::move(hcfg));
    h.host->startup();
    h.endpoint = h.host->broker_endpoint();
    h.pubkey   = h.host->broker_pubkey();
    return h;
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
