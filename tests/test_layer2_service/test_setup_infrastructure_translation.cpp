/**
 * @file test_setup_infrastructure_translation.cpp
 * @brief L2 round-trip tests for the config→opts translation layer used by
 *        each role host's setup_infrastructure_().
 *
 * ── Why this test exists (closes TODO #83 / N1) ───────────────────────────
 *
 * Two production bugs shipped in the same week from the SAME spot:
 *   - B5 (2026-05-20): consumer's `shm_name` field never copied from
 *     config → SHM connect failed silently.
 *   - B11 (2026-05-21): consumer/processor's ZMQ fields
 *     (data_transport / zmq_node_endpoint / clear shm_name) never copied
 *     when transport was zmq → build_rx_queue dispatched the SHM path
 *     on a zmq pipeline.
 * Both lived in the inline body of `setup_infrastructure_` — the
 * "config-to-opts translation" layer.  Existing L3 tests
 * (role_api_flexzone_workers.cpp) hand-constructed `RxQueueOptions`
 * directly and never exercised the translation, so neither bug was
 * caught.
 *
 * ── Production round-trip the test exercises ──────────────────────────────
 *
 * Strict deployment-path mirror (per user direction 2026-05-22):
 *
 *   1. **Generate**: `RoleDirectory::init_directory(dir, role_tag, name)`
 *      — the exact library function `plh_role --init` invokes.  Writes
 *      a default JSON config to disk via the registered
 *      `config_template`.
 *
 *   2. **Mutate** (mimics a user editing config to non-default values):
 *      every field the translation copies is set to a deliberately
 *      non-default value.  This catches fields that `--init` leaves at
 *      template defaults — without this step the test would only
 *      exercise the values `--init` happens to set, leaving translation
 *      regressions on the other fields invisible.
 *
 *   3. **Load**: `RoleConfig::load_from_directory(dir, role_tag,
 *      info->config_parser)` — the same path `plh_role` itself takes
 *      (see `src/plh_role/plh_role_main.cpp:249`).
 *
 *   4. **Translate**: `ProducerRoleHost::make_tx_opts(config, ...)` (or
 *      consumer / processor equivalent) — the extracted-from-
 *      setup_infrastructure_ pure function the production
 *      `setup_infrastructure_` now calls.  This is the layer the bugs
 *      lived in.
 *
 *   5. **Assert**: every field of the resulting `TxQueueOptions` /
 *      `RxQueueOptions` matches the value we wrote into the JSON in
 *      step 2.
 *
 * Mutation-sweep coverage: every config field exercised has a
 * non-template-default value.  If a translation regression forgets to
 * copy any single field, the corresponding `EXPECT_EQ` fails.
 *
 * ── Scope ─────────────────────────────────────────────────────────────────
 *
 * 6 tests: (producer, consumer, processor) × (shm, zmq).
 *
 * Schema specs (slot/fz) are passed-through parameters to the
 * translation — NOT config-derived.  We pass empty `SchemaSpec` and
 * assert they propagate unchanged (one-line check; not the focus).
 */

#include "consumer_init.hpp"
#include "consumer_role_host.hpp"
#include "processor_init.hpp"
#include "processor_role_host.hpp"
#include "producer_init.hpp"
#include "producer_role_host.hpp"

#include "utils/config/role_config.hpp"
#include "utils/role_directory.hpp"
#include "utils/role_registry.hpp"

#include "plh_datahub.hpp"

// Lifecycle modules needed by RoleConfig::load_from_directory →
// JsonConfig + FileLock + Logger.  Without this, `JsonConfig::JsonConfig()`
// panics with "created before its module was initialized via LifecycleManager".
#include "binary_lifecycle.h"
#include "utils/crypto_utils.hpp"
#include "utils/file_lock.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::config::RoleConfig;
using pylabhub::utils::RoleDirectory;
using pylabhub::utils::RoleRegistry;
using pylabhub::utils::RoleRuntimeInfo;

// Binary-wide LifecycleGuard.  Production deployment lives inside
// `LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules())`
// (see src/plh_role/plh_role_main.cpp:236).  Tests mirror the subset of
// modules `RoleConfig::load_from_directory` actually touches.
PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule(),
    pylabhub::utils::FileLock::GetLifecycleModule(),
    pylabhub::utils::JsonConfig::GetLifecycleModule(),
    pylabhub::crypto::GetLifecycleModule())

namespace
{

nlohmann::json read_json(const fs::path &p)
{
    std::ifstream f(p);
    return nlohmann::json::parse(f);
}

void write_json(const fs::path &p, const nlohmann::json &j)
{
    std::ofstream f(p);
    f << j.dump(2);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Fixture
// ─────────────────────────────────────────────────────────────────────────────

class SetupInfrastructureTranslationTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        static bool registered = false;
        if (registered)
            return;
        // Two-stage registration mirrors what plh_role binary does:
        //   register_*_init     → RoleDirectory entry (init_directory).
        //   register_*_runtime  → RoleRegistry entry (load_from_directory parser).
        // Both are required for the production round-trip.
        pylabhub::producer::register_producer_init();
        pylabhub::consumer::register_consumer_init();
        pylabhub::processor::register_processor_init();
        pylabhub::producer::register_producer_runtime();
        pylabhub::consumer::register_consumer_runtime();
        pylabhub::processor::register_processor_runtime();
        registered = true;
    }

  protected:
    fs::path unique_dir(const char *prefix)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_translate_" + std::string(prefix) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p;
    }

    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    /// Run the deployment round-trip:
    ///   init_directory(role_tag) → modify JSON with `mutate` → load_from_directory.
    /// Returns the loaded RoleConfig.  Test caller then runs the
    /// role-specific make_*_opts + asserts fields.
    template <typename Mutate>
    RoleConfig generate_and_load(const std::string &role_tag,
                                  const std::string &cfg_filename,
                                  const std::string &name,
                                  Mutate            &&mutate)
    {
        fs::path dir = unique_dir(role_tag.c_str());
        const int rc = RoleDirectory::init_directory(dir, role_tag, name);
        EXPECT_EQ(rc, 0) << "init_directory failed for role='" << role_tag << "'";

        const fs::path cfg_path = dir / cfg_filename;
        EXPECT_TRUE(fs::exists(cfg_path))
            << "init_directory did not write " << cfg_path;

        // ── Mutate the on-disk JSON (mimics user editing the file) ──
        nlohmann::json j = read_json(cfg_path);
        mutate(j);
        write_json(cfg_path, j);

        // ── Production loader path (mirrors src/plh_role/plh_role_main.cpp:249) ──
        const RoleRuntimeInfo *info = RoleRegistry::get_runtime(role_tag);
        EXPECT_NE(info, nullptr)
            << "RoleRegistry::get_runtime('" << role_tag << "') returned null";
        return RoleConfig::load_from_directory(
            dir.string(), info->role_tag.c_str(), info->config_parser);
    }

  private:
    std::vector<fs::path> paths_to_clean_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Producer × SHM
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SetupInfrastructureTranslationTest, Producer_ShmTransport_AllFieldsCopied)
{
    // Mutate every translation-relevant field to a deliberately non-default
    // value.  If make_tx_opts forgets to copy any one, the corresponding
    // EXPECT_EQ below fails.
    const auto cfg = generate_and_load(
        "producer", "producer.json", "TestProd",
        [](nlohmann::json &j) {
            j["out_channel"]            = "test.prod.channel";
            j["out_transport"]          = "shm";
            j["out_shm_enabled"]        = true;
            j["out_shm_slot_count"]     = 64;          // template = 8
            j["out_shm_secret"]         = 4242424242u; // template omits → 0
            j["out_shm_sync_policy"]    = "sequential_sync"; // template = "sequential"
            j["checksum"]               = "none";      // template = "enforced"
            j["flexzone_checksum"]      = false;       // template = true
        });

    const pylabhub::hub::SchemaSpec slot_spec, fz_spec;
    const bool has_tx_fz = true;

    const auto opts = pylabhub::producer::ProducerRoleHost::make_tx_opts(
        cfg, slot_spec, fz_spec, has_tx_fz);

    // Transport dispatch.
    EXPECT_TRUE(opts.has_shm) << "shm.enabled=true must propagate to has_shm";
    EXPECT_TRUE(opts.data_transport.empty() || opts.data_transport == "shm")
        << "SHM transport must NOT set data_transport='zmq'";

    // SHM block — every field the translation copies under has_shm.
    EXPECT_EQ(opts.shm_config.shared_secret,        4242424242u);
    EXPECT_EQ(opts.shm_config.ring_buffer_capacity, 64u);
    EXPECT_EQ(opts.shm_config.policy,
              pylabhub::hub::DataBlockPolicy::RingBuffer);
    // Sync policy — exercises a flag --init doesn't write (template
    // omits out_shm_sync_policy; we set "sequential_sync" above).  If
    // translation forgets to copy `shm.sync_policy → shm_config.
    // consumer_sync_policy`, this fails.
    EXPECT_EQ(opts.shm_config.consumer_sync_policy,
              pylabhub::hub::ConsumerSyncPolicy::Sequential_sync);
    // physical_page_size is computed from the system; assert it's set
    // (i.e. not the zero-init default) by checking inequality with the
    // enum's zero value.
    EXPECT_NE(static_cast<int>(opts.shm_config.physical_page_size), 0);

    // Checksum (set from config.checksum().policy).
    EXPECT_EQ(opts.checksum_policy, cfg.checksum().policy);
    EXPECT_EQ(opts.shm_config.checksum_policy, cfg.checksum().policy);
    // flexzone_checksum = (config.checksum().flexzone == false) && has_fz=true → false.
    EXPECT_FALSE(opts.flexzone_checksum);

    // Sanity: schema specs propagate unchanged (pass-through, not config-derived).
    EXPECT_EQ(opts.slot_spec.fields.size(), slot_spec.fields.size());
    EXPECT_EQ(opts.fz_spec.fields.size(),   fz_spec.fields.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// Producer × ZMQ (catches the B11 class of bug for the producer side)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SetupInfrastructureTranslationTest, Producer_ZmqTransport_AllFieldsCopied)
{
    const auto cfg = generate_and_load(
        "producer", "producer.json", "TestProdZmq",
        [](nlohmann::json &j) {
            j["out_channel"]              = "test.prod.zmq.channel";
            j["out_transport"]            = "zmq";
            // ZMQ fields are NOT in the producer template — set all of them.
            j["out_zmq_endpoint"]         = "tcp://127.0.0.1:5599";
            j["out_zmq_bind"]             = true;
            j["out_zmq_buffer_depth"]     = 512;
            j["out_zmq_overflow_policy"]  = "block";
            j["out_shm_enabled"]          = false;
            j["checksum"]                 = "enforced";
            j["flexzone_checksum"]        = true;
        });

    const pylabhub::hub::SchemaSpec slot_spec, fz_spec;

    const auto opts = pylabhub::producer::ProducerRoleHost::make_tx_opts(
        cfg, slot_spec, fz_spec, /*has_tx_fz=*/true);

    // Transport dispatch — these are the fields B11's mirror would miss.
    EXPECT_FALSE(opts.has_shm)
        << "ZMQ transport must override has_shm to false";
    EXPECT_EQ(opts.data_transport, "zmq")
        << "data_transport='zmq' must be set (mirror of B11 on producer side)";
    EXPECT_EQ(opts.zmq_node_endpoint, "tcp://127.0.0.1:5599");
    EXPECT_TRUE(opts.zmq_bind);
    EXPECT_EQ(opts.zmq_buffer_depth, 512);
    EXPECT_EQ(opts.zmq_overflow_policy,
              pylabhub::hub::OverflowPolicy::Block);

    // Checksum.
    EXPECT_EQ(opts.checksum_policy, cfg.checksum().policy);
    EXPECT_TRUE(opts.flexzone_checksum);  // (true && has_fz=true)
}

// ─────────────────────────────────────────────────────────────────────────────
// Consumer × SHM (the original B5 site — shm_name MUST come from config)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SetupInfrastructureTranslationTest, Consumer_ShmTransport_AllFieldsCopied)
{
    const std::string ch = "test.cons.shm.channel";
    const auto cfg = generate_and_load(
        "consumer", "consumer.json", "TestCons",
        [&ch](nlohmann::json &j) {
            j["in_channel"]          = ch;
            j["in_transport"]        = "shm";
            j["in_shm_enabled"]      = true;
            j["in_shm_secret"]       = 7777777777u;
            j["checksum"]            = "none";    // not the template default
            j["flexzone_checksum"]   = true;
        });

    const pylabhub::hub::SchemaSpec slot_spec, fz_spec;

    const auto opts = pylabhub::consumer::ConsumerRoleHost::make_rx_opts(
        cfg, slot_spec, fz_spec, /*has_rx_fz=*/true);

    // The B5 site: shm_name MUST equal the configured channel.
    EXPECT_EQ(opts.shm_name, ch)
        << "B5 regression: shm_name must equal in_channel for the SHM path";
    EXPECT_EQ(opts.shm_shared_secret, 7777777777u);

    // SHM transport must NOT activate the ZMQ branch.
    EXPECT_NE(opts.data_transport, "zmq");
    EXPECT_TRUE(opts.zmq_node_endpoint.empty())
        << "ZMQ endpoint should be unset on SHM path";

    EXPECT_EQ(opts.checksum_policy, cfg.checksum().policy);
    EXPECT_TRUE(opts.flexzone_checksum);
}

// ─────────────────────────────────────────────────────────────────────────────
// Consumer × ZMQ (the original B11 site — every ZMQ field must propagate
//                 + shm_name MUST be cleared)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SetupInfrastructureTranslationTest, Consumer_ZmqTransport_AllFieldsCopied)
{
    const auto cfg = generate_and_load(
        "consumer", "consumer.json", "TestConsZmq",
        [](nlohmann::json &j) {
            j["in_channel"]            = "test.cons.zmq.channel";
            j["in_transport"]          = "zmq";
            j["in_zmq_endpoint"]       = "tcp://127.0.0.1:5601";
            j["in_zmq_buffer_depth"]   = 1024;
            j["in_shm_enabled"]        = false;
            j["checksum"]              = "enforced";
            j["flexzone_checksum"]     = false;
        });

    const pylabhub::hub::SchemaSpec slot_spec, fz_spec;

    const auto opts = pylabhub::consumer::ConsumerRoleHost::make_rx_opts(
        cfg, slot_spec, fz_spec, /*has_rx_fz=*/true);

    // B11 regression — every ZMQ field MUST propagate.
    EXPECT_EQ(opts.data_transport, "zmq")
        << "B11 regression: data_transport must be 'zmq' for ZMQ pipeline";
    EXPECT_EQ(opts.zmq_node_endpoint, "tcp://127.0.0.1:5601")
        << "B11 regression: zmq_node_endpoint must come from config";
    EXPECT_EQ(opts.zmq_buffer_depth, 1024);

    // B5 corollary — when transport is ZMQ, shm_name MUST be cleared.
    EXPECT_TRUE(opts.shm_name.empty())
        << "ZMQ path must clear shm_name (build_rx_queue uses shm_name "
           "to dispatch SHM; a non-empty value misroutes to SHM)";
    EXPECT_EQ(opts.shm_shared_secret, 0u);

    EXPECT_EQ(opts.checksum_policy, cfg.checksum().policy);
    EXPECT_FALSE(opts.flexzone_checksum);
}

// ─────────────────────────────────────────────────────────────────────────────
// Processor × SHM (has BOTH directions — rx AND tx)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SetupInfrastructureTranslationTest, Processor_ShmTransport_AllFieldsCopied)
{
    const std::string in_ch  = "test.proc.in.shm.channel";
    const std::string out_ch = "test.proc.out.shm.channel";

    const auto cfg = generate_and_load(
        "processor", "processor.json", "TestProc",
        [&](nlohmann::json &j) {
            j["in_channel"]           = in_ch;
            j["out_channel"]          = out_ch;
            j["in_transport"]         = "shm";
            j["out_transport"]        = "shm";
            j["in_shm_enabled"]       = true;
            j["in_shm_secret"]        = 1111111111u;
            j["out_shm_enabled"]      = true;
            j["out_shm_slot_count"]   = 128;
            j["out_shm_secret"]       = 2222222222u;
            j["out_shm_sync_policy"]  = "latest_only";  // template = "sequential"
            j["checksum"]             = "enforced";
            j["flexzone_checksum"]    = true;
        });

    const pylabhub::hub::SchemaSpec slot_spec, fz_spec;

    // ── Rx side ──
    const auto rx = pylabhub::processor::ProcessorRoleHost::make_rx_opts(
        cfg, slot_spec, fz_spec, /*has_rx_fz=*/true);
    EXPECT_EQ(rx.shm_name, in_ch)
        << "Processor.rx — B5 regression: shm_name must equal in_channel";
    EXPECT_EQ(rx.shm_shared_secret, 1111111111u);
    EXPECT_NE(rx.data_transport, "zmq");
    EXPECT_EQ(rx.checksum_policy, cfg.checksum().policy);
    EXPECT_TRUE(rx.flexzone_checksum);

    // ── Tx side ──
    const auto tx = pylabhub::processor::ProcessorRoleHost::make_tx_opts(
        cfg, slot_spec, fz_spec, /*has_tx_fz=*/true);
    EXPECT_TRUE(tx.has_shm);
    EXPECT_EQ(tx.shm_config.shared_secret,        2222222222u);
    EXPECT_EQ(tx.shm_config.ring_buffer_capacity, 128u);
    EXPECT_EQ(tx.shm_config.policy,
              pylabhub::hub::DataBlockPolicy::RingBuffer);
    EXPECT_EQ(tx.shm_config.consumer_sync_policy,
              pylabhub::hub::ConsumerSyncPolicy::Latest_only);
    EXPECT_NE(static_cast<int>(tx.shm_config.physical_page_size), 0);
    EXPECT_EQ(tx.checksum_policy, cfg.checksum().policy);
    EXPECT_EQ(tx.shm_config.checksum_policy, cfg.checksum().policy);
    EXPECT_TRUE(tx.flexzone_checksum);
}

// ─────────────────────────────────────────────────────────────────────────────
// Processor × ZMQ — dual-hub ZMQ pipeline (the live demo scenario)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SetupInfrastructureTranslationTest, Processor_ZmqTransport_AllFieldsCopied)
{
    const auto cfg = generate_and_load(
        "processor", "processor.json", "TestProcZmq",
        [](nlohmann::json &j) {
            j["in_channel"]               = "test.proc.zmq.in";
            j["out_channel"]              = "test.proc.zmq.out";
            j["in_transport"]             = "zmq";
            j["in_zmq_endpoint"]          = "tcp://127.0.0.1:5701";
            j["in_zmq_buffer_depth"]      = 256;
            j["in_shm_enabled"]           = false;
            j["out_transport"]            = "zmq";
            j["out_zmq_endpoint"]         = "tcp://127.0.0.1:5702";
            j["out_zmq_bind"]             = true;
            j["out_zmq_buffer_depth"]     = 512;
            j["out_zmq_overflow_policy"]  = "drop";
            j["out_shm_enabled"]          = false;
            j["checksum"]                 = "enforced";
            j["flexzone_checksum"]        = false;
        });

    const pylabhub::hub::SchemaSpec slot_spec, fz_spec;

    // ── Rx side (B11 regression) ──
    const auto rx = pylabhub::processor::ProcessorRoleHost::make_rx_opts(
        cfg, slot_spec, fz_spec, /*has_rx_fz=*/true);
    EXPECT_EQ(rx.data_transport, "zmq");
    EXPECT_EQ(rx.zmq_node_endpoint, "tcp://127.0.0.1:5701");
    EXPECT_EQ(rx.zmq_buffer_depth, 256);
    EXPECT_TRUE(rx.shm_name.empty())
        << "Processor.rx — B11 regression: shm_name MUST be cleared on ZMQ path";
    EXPECT_EQ(rx.shm_shared_secret, 0u);

    // ── Tx side ──
    const auto tx = pylabhub::processor::ProcessorRoleHost::make_tx_opts(
        cfg, slot_spec, fz_spec, /*has_tx_fz=*/true);
    EXPECT_FALSE(tx.has_shm);
    EXPECT_EQ(tx.data_transport, "zmq");
    EXPECT_EQ(tx.zmq_node_endpoint, "tcp://127.0.0.1:5702");
    EXPECT_TRUE(tx.zmq_bind);
    EXPECT_EQ(tx.zmq_buffer_depth, 512);
    EXPECT_EQ(tx.zmq_overflow_policy,
              pylabhub::hub::OverflowPolicy::Drop);
    EXPECT_EQ(tx.checksum_policy, cfg.checksum().policy);
    EXPECT_FALSE(tx.flexzone_checksum);
}
