/**
 * @file role_host_frame_shm_workers.cpp
 * @brief Subprocess workers for `test_role_host_frame_shm.cpp`
 *        (HEP-CORE-0041 task #270 L2 coverage).
 *
 * Four scenarios exercised, all running inside `run_gtest_worker`:
 *
 *   - `prepare_zmq_noop`       — ZMQ TX → prepare_tx_capability_ is a
 *                                no-op (returns true, transport stays
 *                                null).
 *   - `prepare_shm_happy`      — SHM TX → transport non-null, fd > 0.
 *   - `spawn_without_transport`— spawn before prepare → false + nulls.
 *   - `spawn_and_cleanup`      — full cycle: prepare + spawn + cleanup.
 *
 * Module dependencies (all installed via `run_gtest_worker`):
 *   - Logger / FileLock / JsonConfig  — base lifecycle
 *   - SecureMemorySubsystem + KeyStore — for role seckey access in
 *                                       spawn_shm_auth_listener_
 *   - ZMQ context                      — ThreadManager init path
 *
 * KeyStore seeding (HEP-CORE-0040 §172) — `CurveKeyStoreFixture` is
 * constructed with synthetic curve keys; identity `kRoleIdentityName`
 * is added explicitly so the production seckey-accessor lambda
 * inside `spawn_shm_auth_listener_` resolves.
 */
#include "curve_test_setup.h"
#include "role_host_frame_test_shim.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include "producer/producer_init.hpp"
#include "producer/producer_fields.hpp"

#include "utils/data_block_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/role_directory.hpp"
#include "utils/role_host_frame.hpp"
#include "utils/role_presence.hpp"
#include "utils/role_uid.hpp"
#include "utils/schema_types.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_memory_subsystem.hpp"
#include "utils/security/shm_capability_channel.hpp"
#include "utils/zmq_context.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using pylabhub::config::RoleConfig;
using pylabhub::scripting::Presence;
using pylabhub::scripting::RoleHostFrameConfig;
using pylabhub::scripting::RoleKind;
using pylabhub::tests::RoleHostFrameTestShim;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;
using pylabhub::utils::RoleDirectory;

namespace pylabhub::tests::pattern4
{

namespace
{

// ── Shared test-fixture helpers ─────────────────────────────────────────────

/// Register the producer role init handler exactly once per
/// subprocess.  Mirrors `register_producer_once` in
/// `role_host_base_workers.cpp` — `RoleDirectory::init_directory`
/// looks up the role by name in the registry, so the lookup MUST be
/// seeded before the first `init_directory("producer", ...)` call.
void register_producer_once()
{
    static bool registered = false;
    if (registered)
        return;
    pylabhub::producer::register_producer_init();
    registered = true;
}

/// Build a producer RoleConfig via `init_directory` + load.  The
/// default producer template is SHM TX (`out_transport=shm`,
/// `out_shm_enabled=true`, `out_channel="lab.my.channel"`); the role
/// host's worker_main_ never runs in these tests so the auth.keyfile
/// vault path on disk is irrelevant.
RoleConfig build_producer_config(const std::string &dir)
{
    register_producer_once();
    if (RoleDirectory::init_directory(dir, "producer", "P") != 0)
        throw std::runtime_error("init_directory failed for SHM test");
    return RoleConfig::load_from_directory(
        dir, "producer", pylabhub::producer::parse_producer_fields);
}

/// Construct the synthetic curve setup + the
/// `CurveKeyStoreFixture` that owns `SecureMemorySubsystem` +
/// `KeyStore` via RAII (HEP-CORE-0040 §4.5/§5.1 — at most one per
/// process).  Caller MUST keep the returned fixture alive for the
/// duration of the test, otherwise KeyStore is destroyed mid-test.
/// Mirrors the seeding pattern from
/// `pattern4_consumer_lifecycle_workers.cpp:246-250`.
struct KeystoreFixture
{
    pylabhub::tests::CurveSetup setup;
    std::unique_ptr<pylabhub::tests::CurveKeyStoreFixture> fixture;
};

KeystoreFixture
seed_keystore_for_role(const std::string &scope_tag,
                        const std::string &role_uid)
{
    KeystoreFixture out;
    out.setup   = pylabhub::tests::make_curve_setup({role_uid});
    out.fixture = std::make_unique<pylabhub::tests::CurveKeyStoreFixture>(
        scope_tag, role_uid, out.setup);
    // Seed the role-identity name the production accessor reads.
    pylabhub::tests::CurveKeyStoreFixture::add_identity(
        pylabhub::utils::security::kRoleIdentityName,
        out.setup.role_keys.at(role_uid));
    return out;
}

/// Build a minimal slot SchemaSpec (one uint32 field) — enough for
/// `datablock_layout_total_size` to return non-zero so the L1
/// transport allocation succeeds.
hub::SchemaSpec minimal_slot_spec()
{
    hub::SchemaSpec spec;
    spec.has_schema = true;
    spec.packing    = "aligned";
    spec.fields.push_back(hub::FieldDef{"value", "uint32", 1u, 0u});
    return spec;
}

/// Build a `PresenceSeed` (primitives the shim reconstructs a fresh
/// move-only `Presence` from inside `build_presences_`).
RoleHostFrameTestShim::PresenceSeed
build_producer_presence(const std::string &channel,
                        const hub::SchemaSpec &slot_spec)
{
    RoleHostFrameTestShim::PresenceSeed seed;
    seed.channel   = channel;
    seed.role_kind = RoleKind::Producer;
    seed.slot_spec = slot_spec;
    return seed;
}

/// Build SHM TX options with the standard ring config.  Mirrors
/// `make_tx_opts` for SHM channels — same shape `make_tx_opts`
/// would emit if the role config selected SHM (which the producer
/// template does by default).
hub::TxQueueOptions make_shm_tx_opts(const hub::SchemaSpec &slot_spec)
{
    hub::TxQueueOptions opts;
    opts.has_shm                          = true;
    opts.data_transport                   = "shm";
    opts.slot_spec                        = slot_spec;
    opts.shm_config.ring_buffer_capacity  = 4;
    opts.shm_config.physical_page_size    = hub::system_page_size();
    opts.shm_config.policy                = hub::DataBlockPolicy::RingBuffer;
    opts.shm_config.consumer_sync_policy  = hub::ConsumerSyncPolicy::Sequential;
    opts.shm_config.checksum_policy       = hub::ChecksumPolicy::None;
    return opts;
}

/// Build ZMQ TX options (negative control for the no-op prepare
/// path).  No SHM bits set.
hub::TxQueueOptions make_zmq_tx_opts(const hub::SchemaSpec &slot_spec)
{
    hub::TxQueueOptions opts;
    opts.has_shm           = false;
    opts.data_transport    = "zmq";
    opts.slot_spec         = slot_spec;
    opts.zmq_node_endpoint = "tcp://127.0.0.1:0";
    opts.zmq_bind          = true;
    return opts;
}

/// Wait (up to @p timeout) for @p pred to return true.  Used to wait
/// for the shim's worker thread to enter its wait loop after
/// `startup_()`.
template <typename F>
bool wait_for(F pred, std::chrono::milliseconds timeout =
                          std::chrono::milliseconds{1000})
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// ── Module accessors for run_gtest_worker ───────────────────────────────────
//
// SecureMemorySubsystem + KeyStore are NOT lifecycle modules —
// they're RAII instances (HEP-CORE-0040 §4.5/§5.1).  Tests construct
// them via `CurveKeyStoreFixture` inside the worker body.

static auto logger_mod()      { return Logger::GetLifecycleModule(); }
static auto file_lock_mod()   { return FileLock::GetLifecycleModule(); }
static auto json_config_mod() { return JsonConfig::GetLifecycleModule(); }
static auto zmq_mod()         { return pylabhub::hub::GetZMQContextModule(); }

} // namespace

// ─── Scenario: ZMQ TX — prepare_tx_capability_ is a no-op ───────────────────

int role_host_frame_shm_prepare_zmq_noop(const char *dir_arg)
{
    return run_gtest_worker(
        [&]() {
            const fs::path dir = dir_arg;
            const std::string role_uid = pylabhub::scripting::make_role_uid(
                pylabhub::scripting::RoleUidTag::Producer, "rhfshm", 1u);
            auto ks = seed_keystore_for_role("rhfshm.test", role_uid);

            auto cfg = build_producer_config(dir.string());

            const auto slot_spec = minimal_slot_spec();
            auto presence = build_producer_presence("test.zmq.noop", slot_spec);
            RoleHostFrameConfig frame_cfg{"prod", "producer", "on_produce"};
            std::atomic<bool> shutdown{false};
            auto shim = std::make_unique<RoleHostFrameTestShim>(
                std::move(cfg), &shutdown, std::move(frame_cfg),
                std::move(presence));

            auto opts = make_zmq_tx_opts(slot_spec);
            const std::string channel = "test.zmq.noop";

            ASSERT_TRUE(shim->test_prepare_tx_capability(opts, channel))
                << "prepare_tx_capability_ must return true on ZMQ (no-op "
                   "early return at role_host_frame.cpp:365)";

            EXPECT_EQ(shim->test_shm_transport(), nullptr)
                << "ZMQ TX must not allocate an IShmCapabilityProducer "
                   "(the early-return branch leaves shm_transport_ null)";
            EXPECT_EQ(opts.shm_capability_fd, -1)
                << "ZMQ TX must not write to opts.shm_capability_fd "
                   "(default sentinel from role_api_base.hpp:114)";
        },
        "role_host_frame_shm.prepare_zmq_noop",
        logger_mod(), file_lock_mod(), json_config_mod(),
        zmq_mod());
}

// ─── Scenario: SHM TX happy path — transport bound, fd populated ────────────

int role_host_frame_shm_prepare_shm_happy(const char *dir_arg)
{
    return run_gtest_worker(
        [&]() {
            const fs::path dir = dir_arg;
            const std::string role_uid = pylabhub::scripting::make_role_uid(
                pylabhub::scripting::RoleUidTag::Producer, "rhfshm", 2u);
            auto ks = seed_keystore_for_role("rhfshm.test", role_uid);

            auto cfg = build_producer_config(dir.string());

            const auto slot_spec = minimal_slot_spec();
            auto presence = build_producer_presence("test.shm.happy", slot_spec);
            RoleHostFrameConfig frame_cfg{"prod", "producer", "on_produce"};
            std::atomic<bool> shutdown{false};
            auto shim = std::make_unique<RoleHostFrameTestShim>(
                std::move(cfg), &shutdown, std::move(frame_cfg),
                std::move(presence));

            auto opts = make_shm_tx_opts(slot_spec);
            const std::string channel = "test.shm.happy";

            ASSERT_TRUE(shim->test_prepare_tx_capability(opts, channel))
                << "prepare_tx_capability_ must succeed for SHM TX";

            EXPECT_NE(shim->test_shm_transport(), nullptr)
                << "Successful prepare must populate shm_transport_";
            EXPECT_GT(opts.shm_capability_fd, 0)
                << "Successful prepare must populate opts.shm_capability_fd "
                   "from IShmCapabilityProducer::borrow_fd() (HEP-CORE-0041 "
                   "§6.3)";
        },
        "role_host_frame_shm.prepare_shm_happy",
        logger_mod(), file_lock_mod(), json_config_mod(),
        zmq_mod());
}

// ─── Scenario: spawn without prior prepare → returns false ──────────────────

int role_host_frame_shm_spawn_without_transport(const char *dir_arg)
{
    return run_gtest_worker(
        [&]() {
            const fs::path dir = dir_arg;
            const std::string role_uid = pylabhub::scripting::make_role_uid(
                pylabhub::scripting::RoleUidTag::Producer, "rhfshm", 3u);
            auto ks = seed_keystore_for_role("rhfshm.test", role_uid);

            auto cfg = build_producer_config(dir.string());

            const auto slot_spec = minimal_slot_spec();
            auto presence = build_producer_presence("test.no.xport", slot_spec);
            RoleHostFrameConfig frame_cfg{"prod", "producer", "on_produce"};
            std::atomic<bool> shutdown{false};
            auto shim = std::make_unique<RoleHostFrameTestShim>(
                std::move(cfg), &shutdown, std::move(frame_cfg),
                std::move(presence));

            // Bring api_ up via startup_() because spawn_shm_auth_listener_
            // touches api().thread_manager() / api().uid().  Even though
            // we expect false return (transport null), the protected
            // method body dereferences api() before checking the
            // pre-condition would have failed via assert — so we still
            // need a valid lifecycle.
            shim->startup_();
            ASSERT_TRUE(wait_for([&] {
                return shim->worker_in_wait_loop();
            })) << "Shim worker_main_ did not enter wait loop after startup_";

            EXPECT_FALSE(shim->test_spawn_shm_auth_listener())
                << "spawn_shm_auth_listener_ must return false when "
                   "shm_transport_ is null (pre-condition violation; "
                   "role_host_frame.cpp:488-494)";
            EXPECT_EQ(shim->test_shm_acceptor(), nullptr);
            EXPECT_EQ(shim->test_shm_orchestrator(), nullptr);

            shim->signal_test_complete();
            shim->shutdown_();
        },
        "role_host_frame_shm.spawn_without_transport",
        logger_mod(), file_lock_mod(), json_config_mod(),
        zmq_mod());
}

// ─── Scenario: prepare → spawn → cleanup full cycle ─────────────────────────

int role_host_frame_shm_spawn_and_cleanup(const char *dir_arg)
{
    return run_gtest_worker(
        [&]() {
            const fs::path dir = dir_arg;
            const std::string role_uid = pylabhub::scripting::make_role_uid(
                pylabhub::scripting::RoleUidTag::Producer, "rhfshm", 4u);
            auto ks = seed_keystore_for_role("rhfshm.test", role_uid);

            auto cfg = build_producer_config(dir.string());

            const auto slot_spec = minimal_slot_spec();
            // Channel name must match cfg.out_channel() so
            // spawn_shm_auth_listener_'s call to this->config().out_channel()
            // and our test_prepare_tx_capability(opts, channel) agree.
            const std::string channel = cfg.out_channel();
            auto presence = build_producer_presence(channel, slot_spec);
            RoleHostFrameConfig frame_cfg{"prod", "producer", "on_produce"};
            std::atomic<bool> shutdown{false};
            auto shim = std::make_unique<RoleHostFrameTestShim>(
                std::move(cfg), &shutdown, std::move(frame_cfg),
                std::move(presence));

            shim->startup_();
            ASSERT_TRUE(wait_for([&] {
                return shim->worker_in_wait_loop();
            })) << "Shim worker_main_ did not enter wait loop";

            // Step 1: prepare_tx_capability_ — SHM transport bound.
            auto opts = make_shm_tx_opts(slot_spec);
            ASSERT_TRUE(shim->test_prepare_tx_capability(opts, channel));
            ASSERT_NE(shim->test_shm_transport(), nullptr);

            // Step 2: spawn_shm_auth_listener_ — orchestrator + thread.
            ASSERT_TRUE(shim->test_spawn_shm_auth_listener());
            EXPECT_NE(shim->test_shm_acceptor(), nullptr);
            EXPECT_NE(shim->test_shm_orchestrator(), nullptr);

            // Step 3: signal complete so the shim's worker_main_
            // exits its wait loop, then run its production-style
            // teardown (api().stop_handler_threads() drains
            // shm_accept_loop, cleanup_tx_capability_() LIFO-releases
            // the bundle).  The contract being pinned is that this
            // sequence completes WITHOUT leaking the accept thread —
            // tracked by the framework's
            // "WORKER UNCLEAN SHUTDOWN: N thread(s) leaked" check
            // in `run_gtest_worker` (any leak → non-zero exit).
            shim->signal_test_complete();
            shim->shutdown_();
        },
        "role_host_frame_shm.spawn_and_cleanup",
        logger_mod(), file_lock_mod(), json_config_mod(),
        zmq_mod());
}

// ─── Dispatcher registration ────────────────────────────────────────────────

int dispatch_role_host_frame_shm(int argc, char **argv)
{
    if (argc < 2) return -1;
    const std::string mode = argv[1];
    const auto        dot  = mode.find('.');
    if (dot == std::string::npos) return -1;
    const std::string module   = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "role_host_frame_shm") return -1;

    if (argc < 3)
    {
        std::fprintf(stderr,
                     "role_host_frame_shm.%s: missing <dir> arg\n",
                     scenario.c_str());
        return 1;
    }
    const char *dir = argv[2];

    if (scenario == "prepare_zmq_noop")
        return role_host_frame_shm_prepare_zmq_noop(dir);
    if (scenario == "prepare_shm_happy")
        return role_host_frame_shm_prepare_shm_happy(dir);
    if (scenario == "spawn_without_transport")
        return role_host_frame_shm_spawn_without_transport(dir);
    if (scenario == "spawn_and_cleanup")
        return role_host_frame_shm_spawn_and_cleanup(dir);

    std::fprintf(stderr, "role_host_frame_shm: unknown scenario '%s'\n",
                 scenario.c_str());
    return 1;
}

struct RoleHostFrameShmRegistrar
{
    RoleHostFrameShmRegistrar()
    {
        ::register_worker_dispatcher(dispatch_role_host_frame_shm);
    }
};

static RoleHostFrameShmRegistrar g_role_host_frame_shm_registrar;

} // namespace pylabhub::tests::pattern4
