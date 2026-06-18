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
 *     (data_transport / clear shm_name; pre-Stage-1D also zmq_node_endpoint)
 *     never copied when transport was zmq → build_rx_queue dispatched
 *     the SHM path on a zmq pipeline.  Stage 1D (#193, 2026-06-15)
 *     retired RxQueueOptions::zmq_node_endpoint — the consumer's
 *     connect target now lives ONLY in producer_peers, populated by
 *     CONSUMER_REG_ACK.producers[] per HEP-CORE-0036 §6.4 + §6.7.
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
            // HEP-CORE-0041 substep 1h (#255) — out_shm_secret retired
            // (parser throws on this field); a separate rejection
            // test below pins the throw shape.  Removed from this
            // round-trip test; the legacy non-zero assertion at the
            // matching EXPECT_EQ below is also dropped.
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
    // HEP-CORE-0041 substep 1h (#255) — shared_secret round-trip
    // assertion removed; the field is no longer settable from config.
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
            // HEP-CORE-0041 1h (#255) — in_shm_secret retired
            j["checksum"]            = "none";    // not the template default
            j["flexzone_checksum"]   = true;
        });

    const pylabhub::hub::SchemaSpec slot_spec, fz_spec;

    const auto opts = pylabhub::consumer::ConsumerRoleHost::make_rx_opts(
        cfg, slot_spec, fz_spec, /*has_rx_fz=*/true);

    // The B5 site: shm_name MUST equal the configured channel.
    EXPECT_EQ(opts.shm_name, ch)
        << "B5 regression: shm_name must equal in_channel for the SHM path";
    // HEP-CORE-0041 1h (#255) — shm_shared_secret no longer settable
    // from config; field defaults to 0 and lives only as legacy
    // runtime residue (deleted in 1i #256).

    // SHM transport must NOT activate the ZMQ branch.
    EXPECT_NE(opts.data_transport, "zmq");
    EXPECT_TRUE(opts.producer_peers.empty())
        << "SHM path must not populate producer_peers (ZMQ-only field)";

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
    // Stage 1D (task #193, 2026-06-15): config no longer carries the
    // consumer's connect target on `RxQueueOptions`.  The broker's
    // `CONSUMER_REG_ACK.producers[]` is the only canonical source
    // (HEP-CORE-0017 §3.3 + HEP-CORE-0036 §6.4 + §6.7).
    EXPECT_TRUE(opts.producer_peers.empty())
        << "Stage 1D: producer_peers must be empty at translation time — "
           "broker fills via apply_consumer_reg_ack later";
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
            // HEP-CORE-0041 1h (#255) — in_shm_secret retired
            j["out_shm_enabled"]      = true;
            j["out_shm_slot_count"]   = 128;
            // HEP-CORE-0041 1h (#255) — out_shm_secret retired
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
    // HEP-CORE-0041 1h (#255) — shm_shared_secret no longer settable
    EXPECT_NE(rx.data_transport, "zmq");
    EXPECT_EQ(rx.checksum_policy, cfg.checksum().policy);
    EXPECT_TRUE(rx.flexzone_checksum);

    // ── Tx side ──
    const auto tx = pylabhub::processor::ProcessorRoleHost::make_tx_opts(
        cfg, slot_spec, fz_spec, /*has_tx_fz=*/true);
    EXPECT_TRUE(tx.has_shm);
    // HEP-CORE-0041 1h (#255) — shared_secret no longer settable from config
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
    // Stage 1D (task #193, 2026-06-15): config no longer carries the
    // consumer's connect target — broker's CONSUMER_REG_ACK.producers[]
    // is the only canonical source per HEP-CORE-0036 §6.4 + §6.7.
    EXPECT_TRUE(rx.producer_peers.empty())
        << "Stage 1D: producer_peers must be empty at translation time";
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

// ─────────────────────────────────────────────────────────────────────────────
// Q2 + Q3 coverage — SchemaSpec field propagation + has_fz=false gate
// ─────────────────────────────────────────────────────────────────────────────
//
// Earlier tests pass empty SchemaSpec and `has_*_fz=true`, leaving two
// gaps:
//   Q2: SchemaSpec propagation — empty-spec assertions are tautological
//       (`0 == 0` always passes even if make_*_opts drops the copy).
//   Q3: `flexzone_checksum = config.flexzone && has_*_fz` is never
//       exercised with `has_*_fz=false`; the AND-gate could be flipped
//       to OR (or to a constant `true`) and existing tests would still
//       pass.
//
// These tests fill both gaps per role.  Each verifies (a) a non-empty
// slot + fz SchemaSpec round-trips with all fields intact, and (b) the
// `has_*_fz=false` path forces `flexzone_checksum=false` even when the
// config bit is true.

namespace
{

/// Build a non-empty test SchemaSpec with two distinguishable fields.
pylabhub::hub::SchemaSpec make_test_spec(const char *first_field_name)
{
    pylabhub::hub::SchemaSpec spec;
    spec.has_schema = true;
    spec.packing    = "aligned";
    {
        pylabhub::hub::FieldDef f;
        f.name        = first_field_name;
        f.type_str    = "f64";
        f.count       = 1;
        f.length      = 0;
        spec.fields.push_back(f);
    }
    {
        pylabhub::hub::FieldDef f;
        f.name        = "tag";
        f.type_str    = "u32";
        f.count       = 1;
        f.length      = 0;
        spec.fields.push_back(f);
    }
    return spec;
}

}  // namespace

TEST_F(SetupInfrastructureTranslationTest,
       Producer_SchemaSpec_Propagates_And_HasFzFalse_ClearsFlexzoneChecksum)
{
    // Config: flexzone_checksum=true so the AND gate's second operand
    // (has_tx_fz) is what determines the result.  Without an explicit
    // has_fz=false case the test cannot tell whether make_tx_opts AND-
    // gates or just copies config.flexzone.
    const auto cfg = generate_and_load(
        "producer", "producer.json", "TestProdQ23",
        [](nlohmann::json &j) {
            j["out_channel"]            = "test.prod.q23";
            j["out_transport"]          = "shm";
            j["out_shm_enabled"]        = true;
            j["out_shm_slot_count"]     = 8;
            // HEP-CORE-0041 1h (#255) — out_shm_secret retired
            j["checksum"]               = "enforced";
            j["flexzone_checksum"]      = true;
        });

    const auto slot_spec = make_test_spec("ts");
    const auto fz_spec   = make_test_spec("cal_offset");

    // (a) has_tx_fz=true — flexzone_checksum should follow config (true).
    {
        const auto opts = pylabhub::producer::ProducerRoleHost::make_tx_opts(
            cfg, slot_spec, fz_spec, /*has_tx_fz=*/true);
        ASSERT_EQ(opts.slot_spec.fields.size(), 2u)
            << "slot_spec.fields must propagate non-empty (Q2)";
        EXPECT_EQ(opts.slot_spec.fields[0].name,     "ts");
        EXPECT_EQ(opts.slot_spec.fields[0].type_str, "f64");
        EXPECT_EQ(opts.slot_spec.fields[1].name,     "tag");
        EXPECT_EQ(opts.slot_spec.fields[1].type_str, "u32");
        ASSERT_EQ(opts.fz_spec.fields.size(), 2u)
            << "fz_spec.fields must propagate non-empty (Q2)";
        EXPECT_EQ(opts.fz_spec.fields[0].name,       "cal_offset");
        EXPECT_TRUE(opts.flexzone_checksum)
            << "with config.flexzone=true and has_tx_fz=true, expect true";
    }

    // (b) has_tx_fz=false — flexzone_checksum must be forced false
    //     regardless of config.flexzone (Q3).
    {
        const auto opts = pylabhub::producer::ProducerRoleHost::make_tx_opts(
            cfg, slot_spec, fz_spec, /*has_tx_fz=*/false);
        EXPECT_FALSE(opts.flexzone_checksum)
            << "has_tx_fz=false must force flexzone_checksum=false (Q3); "
               "config.flexzone=true is irrelevant when there is no fz";
    }
}

TEST_F(SetupInfrastructureTranslationTest,
       Consumer_SchemaSpec_Propagates_And_HasRxFzFalse_ClearsFlexzoneChecksum)
{
    const auto cfg = generate_and_load(
        "consumer", "consumer.json", "TestConsQ23",
        [](nlohmann::json &j) {
            j["in_channel"]             = "test.cons.q23";
            j["in_transport"]           = "shm";
            j["in_shm_enabled"]         = true;
            // HEP-CORE-0041 1h (#255) — in_shm_secret retired
            j["checksum"]               = "enforced";
            j["flexzone_checksum"]      = true;
        });

    const auto slot_spec = make_test_spec("ts");
    const auto fz_spec   = make_test_spec("cal_offset");

    {
        const auto opts = pylabhub::consumer::ConsumerRoleHost::make_rx_opts(
            cfg, slot_spec, fz_spec, /*has_rx_fz=*/true);
        ASSERT_EQ(opts.slot_spec.fields.size(), 2u);
        EXPECT_EQ(opts.slot_spec.fields[0].name, "ts");
        EXPECT_EQ(opts.slot_spec.fields[1].name, "tag");
        ASSERT_EQ(opts.fz_spec.fields.size(), 2u);
        EXPECT_EQ(opts.fz_spec.fields[0].name, "cal_offset");
        EXPECT_TRUE(opts.flexzone_checksum);
    }

    {
        const auto opts = pylabhub::consumer::ConsumerRoleHost::make_rx_opts(
            cfg, slot_spec, fz_spec, /*has_rx_fz=*/false);
        EXPECT_FALSE(opts.flexzone_checksum)
            << "has_rx_fz=false must force flexzone_checksum=false (Q3)";
    }
}

TEST_F(SetupInfrastructureTranslationTest,
       Processor_SchemaSpec_Propagates_And_HasFzFalse_BothSides)
{
    const auto cfg = generate_and_load(
        "processor", "processor.json", "TestProcQ23",
        [](nlohmann::json &j) {
            j["in_channel"]              = "test.proc.in.q23";
            j["out_channel"]             = "test.proc.out.q23";
            j["in_transport"]            = "shm";
            j["out_transport"]           = "shm";
            j["in_shm_enabled"]          = true;
            j["out_shm_enabled"]         = true;
            // HEP-CORE-0041 1h (#255) — *_shm_secret retired
            j["checksum"]                = "enforced";
            j["flexzone_checksum"]       = true;
        });

    const auto in_slot_spec  = make_test_spec("in_ts");
    const auto out_slot_spec = make_test_spec("out_ts");
    const auto in_fz_spec    = make_test_spec("in_cal");
    const auto out_fz_spec   = make_test_spec("out_cal");

    // (a) has_*_fz=true — flexzone_checksum follows config, schemas propagate.
    {
        const auto rx = pylabhub::processor::ProcessorRoleHost::make_rx_opts(
            cfg, in_slot_spec, in_fz_spec, /*has_rx_fz=*/true);
        ASSERT_EQ(rx.slot_spec.fields.size(), 2u);
        EXPECT_EQ(rx.slot_spec.fields[0].name, "in_ts");
        ASSERT_EQ(rx.fz_spec.fields.size(), 2u);
        EXPECT_EQ(rx.fz_spec.fields[0].name, "in_cal");
        EXPECT_TRUE(rx.flexzone_checksum);

        const auto tx = pylabhub::processor::ProcessorRoleHost::make_tx_opts(
            cfg, out_slot_spec, out_fz_spec, /*has_tx_fz=*/true);
        ASSERT_EQ(tx.slot_spec.fields.size(), 2u);
        EXPECT_EQ(tx.slot_spec.fields[0].name, "out_ts");
        ASSERT_EQ(tx.fz_spec.fields.size(), 2u);
        EXPECT_EQ(tx.fz_spec.fields[0].name, "out_cal");
        EXPECT_TRUE(tx.flexzone_checksum);
    }

    // (b) has_*_fz=false on each side independently — verify the
    //     AND-gate fires per side (Q3).
    {
        const auto rx = pylabhub::processor::ProcessorRoleHost::make_rx_opts(
            cfg, in_slot_spec, in_fz_spec, /*has_rx_fz=*/false);
        EXPECT_FALSE(rx.flexzone_checksum)
            << "rx has_rx_fz=false must clear rx flexzone_checksum (Q3)";

        const auto tx = pylabhub::processor::ProcessorRoleHost::make_tx_opts(
            cfg, out_slot_spec, out_fz_spec, /*has_tx_fz=*/false);
        EXPECT_FALSE(tx.flexzone_checksum)
            << "tx has_tx_fz=false must clear tx flexzone_checksum (Q3)";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HEP-CORE-0041 substep 1h (#255) — retired-key rejection
// ─────────────────────────────────────────────────────────────────────────────
// Pin: a config carrying the legacy `out_shm_secret` or `in_shm_secret`
// field is rejected by `reject_retired_keys` (in role_config.cpp)
// BEFORE the generic unknown-key check.  The thrown message must
// carry a recognizable HEP-0041 reference so operators see the
// migration path, not a generic "unknown config key".
//
// Pre-1h these fields were accepted and stamped into the SHM header's
// `shared_secret` byte field; post-1h the parser rejects them at
// schema-validation time.  Substep 1i (#256) deletes the dead
// internal `secret` field on ShmConfig + the
// `apply_master_approval(ack["shm_secret"])` artifact extraction.
TEST_F(SetupInfrastructureTranslationTest, RetiredShmSecretKeysRejected)
{
    EXPECT_THROW(
        {
            (void)generate_and_load(
                "producer", "producer.json", "TestProdRetired",
                [](nlohmann::json &j) {
                    j["out_channel"]     = "test.retired.prod";
                    j["out_transport"]   = "shm";
                    j["out_shm_enabled"] = true;
                    j["out_shm_secret"]  = 4242u; // <- retired
                });
        },
        std::runtime_error);

    EXPECT_THROW(
        {
            (void)generate_and_load(
                "consumer", "consumer.json", "TestConsRetired",
                [](nlohmann::json &j) {
                    j["in_channel"]     = "test.retired.cons";
                    j["in_transport"]   = "shm";
                    j["in_shm_enabled"] = true;
                    j["in_shm_secret"]  = 7777u; // <- retired
                });
        },
        std::runtime_error);

    // Verify the message shape — operators searching the log for
    // "HEP-CORE-0041" must find the rejection.  Pin via try/catch so
    // we can inspect the what() string.
    try
    {
        (void)generate_and_load(
            "producer", "producer.json", "TestProdRetiredMsg",
            [](nlohmann::json &j) {
                j["out_channel"]     = "test.retired.msg";
                j["out_transport"]   = "shm";
                j["out_shm_enabled"] = true;
                j["out_shm_secret"]  = 1u;
            });
        FAIL() << "expected throw on retired out_shm_secret key";
    }
    catch (const std::runtime_error &e)
    {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("out_shm_secret"), std::string::npos)
            << "rejection message must name the retired key; got: " << msg;
        EXPECT_NE(msg.find("HEP-CORE-0041"), std::string::npos)
            << "rejection message must cite HEP-CORE-0041 for the "
               "migration path; got: " << msg;
    }
}
