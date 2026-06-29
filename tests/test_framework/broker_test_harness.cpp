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
#include "utils/role_reg_payload.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/known_roles.hpp"
#include "utils/security/shm_capability_channel.hpp"

#include <unistd.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
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
    // HEP-CORE-0040 §172: the hub identity is read from `key_store()`
    // under `"hub_identity"`.  Caller MUST have a
    // `CurveKeyStoreFixture` in scope BEFORE calling — its ctor
    // seeded the KeyStore from `setup`.  Per HEP-CORE-0035 §2 the
    // CURVE install is unconditional; BrokerService ctor throws if
    // the entry is absent.
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
    // HEP-CORE-0040 §172.  Same contract as `start_direct_broker`:
    // the caller has a `CurveKeyStoreFixture` in scope which has
    // already seeded the process KeyStore with `"hub_identity"`
    // (from `setup.hub`) and one `"role.<uid>"` entry per role uid
    // in `setup.role_keys`.  This helper only READS — it never
    // mutates the KeyStore.
    //
    // The pre-check is loud and specific so a missing fixture
    // surfaces at the call site instead of inside HubHost::startup.
    namespace sec = pylabhub::utils::security;
    if (!sec::key_store_ready() ||
        !sec::key_store().has(sec::kHubIdentityName))
    {
        throw std::logic_error(
            "start_hubhost_broker: KeyStore entry 'hub_identity' is "
            "absent.  Construct `pylabhub::tests::CurveKeyStoreFixture` "
            "in scope BEFORE calling — it owns SMS + KeyStore + the "
            "identity seeding (HEP-CORE-0040 §172).  Same contract "
            "as start_direct_broker; both helpers only read.");
    }

    HubHostBrokerHandle h;
    h.hub_dir = make_unique_hub_dir(hub_name);

    // Step 1 — initialize hub directory.  Writes hub.json template +
    // logging skeleton + vault/ + scripts/ + logs/.
    if (pylabhub::utils::HubDirectory::init_directory(
            h.hub_dir, std::string(hub_name)) != 0)
    {
        throw std::runtime_error(
            "start_hubhost_broker: HubDirectory::init_directory failed "
            "for '" + h.hub_dir.string() + "'");
    }

    // Step 2 — merge j_overrides into hub.json so per-fixture tweaks
    // (ephemeral port, admin off, script disabled, ...) take effect.
    {
        const fs::path hub_json_path = h.hub_dir / "hub.json";
        nlohmann::json j;
        {
            std::ifstream in(hub_json_path);
            if (!in)
                throw std::runtime_error(
                    "start_hubhost_broker: cannot open " +
                    hub_json_path.string() + " after init_directory");
            in >> j;
        }
        if (!j_overrides.empty())
            j.merge_patch(j_overrides);
        {
            std::ofstream out(hub_json_path,
                              std::ios::out | std::ios::trunc);
            out << j.dump(2);
        }
    }

    // Step 3 — write known_roles.json (HEP-CORE-0035 §4.8) via the
    // production `KnownRolesStore::save_to_file` so the file format
    // (version + roles array + atomic-write + 0600 perms) is defined
    // in exactly one place.  No parallel JSON-shape logic in tests.
    {
        pylabhub::utils::security::KnownRolesStore store;
        for (const auto &[uid, kp] : setup.role_keys)
        {
            store.add(make_known_role(uid, kp.public_z85));
        }
        store.save_to_file(h.hub_dir / "vault" / "known_roles.json");
    }

    // Step 4 — load HubConfig from the prepared directory.  We do NOT
    // call `cfg.load_keypair(password)` because the KeyStore already
    // has the identity seeded above.  HubConfig parses `auth.keyfile`
    // as a string but never reads the vault file unless load_keypair
    // is invoked.
    auto cfg =
        pylabhub::config::HubConfig::load_from_directory(h.hub_dir);

    // Step 5 — construct HubHost and start the broker.  `startup()`
    // builds the BrokerService (which checks `key_store().has(
    // "hub_identity")`), wires the admin/script subsystems, and
    // spawns the broker thread.  After startup() returns the broker
    // is listening on `broker_endpoint()`.
    h.host =
        std::make_unique<pylabhub::hub_host::HubHost>(std::move(cfg));
    h.host->startup();
    h.endpoint = h.host->broker_endpoint();
    h.pubkey   = setup.hub.public_z85;
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
                      const std::string &keystore_name)
{
    pylabhub::hub::BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = endpoint;
    cfg.broker_pubkey   = server_pubkey;
    cfg.keystore_name   = keystore_name;
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

// ── REG_REQ / CONSUMER_REG_REQ payload builders ─────────────────────────────

nlohmann::json make_reg_opts(const std::string &channel,
                              const std::string &role_uid,
                              std::optional<uint64_t> producer_pid)
{
    namespace sec = pylabhub::utils::security;
    auto opts = pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = channel,
            .role_uid   = role_uid,
            .role_name  = "test_producer",
            .role_type  = "producer",
            .has_shm    = true,
            .is_zmq_transport  = false,
            .zmq_node_endpoint = {},
            // HEP-CORE-0036 §I10: one pubkey per role_uid — derived from
            // the keystore, not passed by the caller.  Throws
            // `std::out_of_range` if the caller forgot to seed the
            // `CurveKeyStoreFixture` for this uid.
            .zmq_pubkey = std::string{sec::key_store().pubkey(
                pylabhub::tests::role_keystore_name(role_uid))},
            .shm_capability_endpoint =
                sec::default_shm_capability_endpoint(channel),
        });
    opts["producer_pid"] =
        producer_pid.has_value() ? *producer_pid
                                  : static_cast<uint64_t>(::getpid());
    return opts;
}

nlohmann::json make_cons_opts(const std::string &channel,
                               const std::string &consumer_uid,
                               std::optional<uint64_t> consumer_pid)
{
    namespace sec = pylabhub::utils::security;
    auto opts = pylabhub::hub::build_consumer_reg_payload(
        pylabhub::hub::ConsumerRegInputs{
            .channel    = channel,
            .role_uid   = consumer_uid,
            .role_name  = "test_consumer",
            // Same §I10 derivation as `make_reg_opts`.
            .zmq_pubkey = std::string{sec::key_store().pubkey(
                pylabhub::tests::role_keystore_name(consumer_uid))},
        });
    opts["consumer_pid"] =
        consumer_pid.has_value() ? *consumer_pid
                                  : static_cast<uint64_t>(::getpid());
    return opts;
}

nlohmann::json make_reg_opts_with_explicit_pubkey(
    const std::string &channel,
    const std::string &role_uid,
    const std::string &zmq_pubkey,
    std::optional<uint64_t> producer_pid)
{
    namespace sec = pylabhub::utils::security;
    auto opts = pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = channel,
            .role_uid   = role_uid,
            .role_name  = "test_producer",
            .role_type  = "producer",
            .has_shm    = true,
            .is_zmq_transport  = false,
            .zmq_node_endpoint = {},
            .zmq_pubkey = zmq_pubkey,
            .shm_capability_endpoint =
                sec::default_shm_capability_endpoint(channel),
        });
    opts["producer_pid"] =
        producer_pid.has_value() ? *producer_pid
                                  : static_cast<uint64_t>(::getpid());
    return opts;
}

nlohmann::json make_cons_opts_with_explicit_pubkey(
    const std::string &channel,
    const std::string &consumer_uid,
    const std::string &zmq_pubkey,
    std::optional<uint64_t> consumer_pid)
{
    auto opts = pylabhub::hub::build_consumer_reg_payload(
        pylabhub::hub::ConsumerRegInputs{
            .channel    = channel,
            .role_uid   = consumer_uid,
            .role_name  = "test_consumer",
            .zmq_pubkey = zmq_pubkey,
        });
    opts["consumer_pid"] =
        consumer_pid.has_value() ? *consumer_pid
                                  : static_cast<uint64_t>(::getpid());
    return opts;
}

} // namespace pylabhub::tests
