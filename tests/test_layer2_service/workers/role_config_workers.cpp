/**
 * @file role_config_workers.cpp
 * @brief Worker bodies for the RoleConfig unit-test suite (Pattern 3).
 *
 * Each worker takes a parent-provided unique scratch directory, writes a
 * minimal JSON config inside it, calls RoleConfig::load* to parse it, and
 * asserts on the resulting fields. run_gtest_worker owns the LifecycleGuard
 * (Logger + FileLock + JsonConfig).
 *
 * The fixture's helpers (minimal_*_json, write_json, parse_test_producer)
 * move here from the parent test file. Bodies are 1:1 conversions of the
 * original TEST_F bodies — same EXPECTs, same boundaries, same flow.
 *
 * No macros wrap the test bodies: each worker writes its run_gtest_worker
 * call explicitly so compile errors and gtest failure messages report
 * accurate source lines and the structure stays grep-able.
 */
#include "role_config_workers.h"

#include "utils/config/role_config.hpp"
#include "plh_datahub.hpp"

#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <fmt/core.h>
#include <gmock/gmock.h>  // testing::HasSubstr for catch-and-assert pattern
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <any>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using pylabhub::config::RoleConfig;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace role_config
{
namespace
{

/// Writes JSON to <dir>/<filename>; returns the full path.
fs::path write_json(const fs::path &dir, const std::string &filename,
                    const nlohmann::json &j)
{
    const auto path = dir / filename;
    std::ofstream ofs(path);
    ofs << j.dump(2);
    return path;
}

nlohmann::json minimal_producer_json()
{
    return {
        {"producer", {{"uid", "prod.test.u00000001"}, {"name", "TestProd"}}},
        {"out_hub_dir", ""},
        {"out_channel", "test.channel"},
        {"out_transport", "shm"},
        {"out_shm_enabled", true},
        {"out_shm_slot_count", 8},
        {"checksum", "enforced"},
        {"out_slot_schema", {{"fields", {{{"name", "value"}, {"type", "float32"}}}}}},
        {"loop_timing", "fixed_rate"},
        {"target_period_ms", 50.0},
        {"script", {{"type", "python"}, {"path", "."}}},
    };
}

nlohmann::json minimal_consumer_json()
{
    return {
        {"consumer", {{"uid", "cons.test.u00000002"}, {"name", "TestCons"}}},
        {"in_hub_dir", ""},
        {"in_channel", "test.channel"},
        {"in_transport", "shm"},
        {"checksum", "enforced"},
        {"loop_timing", "max_rate"},
        {"script", {{"type", "lua"}, {"path", "."}}},
    };
}

nlohmann::json minimal_processor_json()
{
    return {
        {"processor", {{"uid", "proc.test.u00000003"}, {"name", "TestProc"}}},
        {"in_hub_dir", ""},
        {"out_hub_dir", ""},
        {"in_channel", "raw.data"},
        {"out_channel", "processed.data"},
        {"in_transport", "shm"},
        {"out_transport", "zmq"},
        {"out_zmq_endpoint", "tcp://0.0.0.0:5599"},
        {"out_zmq_bind", true},
        {"out_slot_schema", {{"fields", {{{"name", "result"}, {"type", "float64"}}}}}},
        {"checksum", "enforced"},
        {"loop_timing", "max_rate"},
        {"script", {{"type", "python"}, {"path", "."}}},
    };
}

struct TestProducerFields
{
    nlohmann::json out_slot_schema;
};

std::any parse_test_producer(const nlohmann::json &j, const RoleConfig & /*cfg*/)
{
    TestProducerFields pf;
    pf.out_slot_schema = j.value("out_slot_schema", nlohmann::json{});
    return pf;
}

} // namespace

// ── Producer ────────────────────────────────────────────────────────────────

int load_producer_identity(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.identity().uid, "prod.test.u00000001");
            EXPECT_EQ(cfg.identity().name, "TestProd");
            EXPECT_EQ(cfg.identity().log_level, "info");
        },
        "role_config::load_producer_identity",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_producer_timing(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_DOUBLE_EQ(cfg.timing().period_us, 50000.0);
        },
        "role_config::load_producer_timing",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_producer_script(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.script().type, "python");
            EXPECT_TRUE(cfg.script().type_explicit);
        },
        "role_config::load_producer_script",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_producer_out_channel(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.out_channel(), "test.channel");
            EXPECT_TRUE(cfg.in_channel().empty());
        },
        "role_config::load_producer_out_channel",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_producer_out_transport(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.out_transport().transport, pylabhub::config::Transport::Shm);
            EXPECT_EQ(cfg.out_shm().slot_count, 8u);
            EXPECT_TRUE(cfg.out_shm().enabled);
        },
        "role_config::load_producer_out_transport",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_producer_out_validation(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.checksum().policy, pylabhub::hub::ChecksumPolicy::Enforced);
        },
        "role_config::load_producer_out_validation",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_producer_validation_stop_on_script_error(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["stop_on_script_error"] = true;
            auto path = write_json(dir, "producer.json", j);
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_TRUE(cfg.script().stop_on_script_error);
        },
        "role_config::load_producer_validation_stop_on_script_error",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Consumer ────────────────────────────────────────────────────────────────

int load_consumer_identity(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "consumer.json", minimal_consumer_json());
            auto cfg = RoleConfig::load(path.string(), "consumer");
            EXPECT_EQ(cfg.identity().uid, "cons.test.u00000002");
            EXPECT_EQ(cfg.script().type, "lua");
        },
        "role_config::load_consumer_identity",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_consumer_in_channel(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "consumer.json", minimal_consumer_json());
            auto cfg = RoleConfig::load(path.string(), "consumer");
            EXPECT_EQ(cfg.in_channel(), "test.channel");
            EXPECT_TRUE(cfg.out_channel().empty());
        },
        "role_config::load_consumer_in_channel",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_consumer_in_validation(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "consumer.json", minimal_consumer_json());
            auto cfg = RoleConfig::load(path.string(), "consumer");
            EXPECT_EQ(cfg.checksum().policy, pylabhub::hub::ChecksumPolicy::Enforced);
        },
        "role_config::load_consumer_in_validation",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_consumer_default_timing(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "consumer.json", minimal_consumer_json());
            auto cfg = RoleConfig::load(path.string(), "consumer");
            EXPECT_DOUBLE_EQ(cfg.timing().period_us, 0.0);
            EXPECT_EQ(cfg.timing().loop_timing, pylabhub::LoopTimingPolicy::MaxRate);
        },
        "role_config::load_consumer_default_timing",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Processor ───────────────────────────────────────────────────────────────

int load_processor_dual_channels(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "processor.json", minimal_processor_json());
            auto cfg = RoleConfig::load(path.string(), "processor");
            EXPECT_EQ(cfg.in_channel(), "raw.data");
            EXPECT_EQ(cfg.out_channel(), "processed.data");
        },
        "role_config::load_processor_dual_channels",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_processor_dual_transport(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "processor.json", minimal_processor_json());
            auto cfg = RoleConfig::load(path.string(), "processor");
            EXPECT_EQ(cfg.in_transport().transport, pylabhub::config::Transport::Shm);
            EXPECT_EQ(cfg.out_transport().transport, pylabhub::config::Transport::Zmq);
            EXPECT_EQ(cfg.out_transport().zmq_endpoint, "tcp://0.0.0.0:5599");
            EXPECT_TRUE(cfg.out_transport().zmq_bind);
        },
        "role_config::load_processor_dual_transport",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_processor_dual_validation(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "processor.json", minimal_processor_json());
            auto cfg = RoleConfig::load(path.string(), "processor");
            EXPECT_EQ(cfg.checksum().policy, pylabhub::hub::ChecksumPolicy::Enforced);
        },
        "role_config::load_processor_dual_validation",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Role-specific data ──────────────────────────────────────────────────────

int role_data_producer_fields(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer", parse_test_producer);
            EXPECT_TRUE(cfg.has_role_data());
            const auto &pf = cfg.role_data<TestProducerFields>();
            EXPECT_FALSE(pf.out_slot_schema.empty());
            EXPECT_TRUE(pf.out_slot_schema.contains("fields"));
        },
        "role_config::role_data_producer_fields",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int role_data_no_parser(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_FALSE(cfg.has_role_data());
        },
        "role_config::role_data_no_parser",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int role_data_wrong_type_cast_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer", parse_test_producer);
            struct WrongType {};
            EXPECT_THROW(cfg.role_data<WrongType>(), std::bad_any_cast);
        },
        "role_config::role_data_wrong_type_cast_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Auth ────────────────────────────────────────────────────────────────────

int auth_default_empty(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_TRUE(cfg.auth().keyfile.empty());
            EXPECT_TRUE(cfg.auth().client_pubkey.empty());
            EXPECT_TRUE(cfg.auth().client_seckey.empty());
        },
        "role_config::auth_default_empty",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_keypair_no_keyfile_returns_false(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_FALSE(cfg.load_keypair("dummy-password"));
        },
        "role_config::load_keypair_no_keyfile_returns_false",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Raw JSON / metadata ─────────────────────────────────────────────────────

int raw_json(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            auto path = write_json(dir, "producer.json", j);
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_TRUE(cfg.raw().contains("producer"));
            EXPECT_TRUE(cfg.raw().contains("out_channel"));
            EXPECT_EQ(cfg.raw()["out_channel"], "test.channel");
        },
        "role_config::raw_json",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int role_tag(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.role_tag(), "producer");
        },
        "role_config::role_tag",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int base_dir(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.base_dir(), fs::path(dir));
        },
        "role_config::base_dir",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int load_from_directory(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load_from_directory(dir, "producer");
            EXPECT_EQ(cfg.identity().uid, "prod.test.u00000001");
            EXPECT_EQ(cfg.out_channel(), "test.channel");
        },
        "role_config::load_from_directory",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Validation errors ───────────────────────────────────────────────────────

int file_not_found_throws()
{
    return run_gtest_worker(
        [&]() {
            // Catch the throw explicitly so we can assert on the exception
            // message text — much more reliable than substring-matching log
            // output, and verifies the contract of what message the user
            // gets when the file is missing.
            try {
                RoleConfig::load("/nonexistent/path.json", "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::runtime_error &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("cannot open config file"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr("/nonexistent/path.json"));
            }
        },
        "role_config::file_not_found_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int invalid_script_type_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["script"]["type"] = "ruby";
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::invalid_argument &e) {
                // parse_script_config throws std::invalid_argument with a
                // message naming the offending value.
                EXPECT_THAT(e.what(), ::testing::HasSubstr("script.type"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr("ruby"));
            }
        },
        "role_config::invalid_script_type_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int zmq_transport_missing_endpoint_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["out_transport"] = "zmq";
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::invalid_argument &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("out_zmq_endpoint"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr("required"));
            }
        },
        "role_config::zmq_transport_missing_endpoint_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int zmq_transport_valid(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["out_transport"] = "zmq";
            j["out_zmq_endpoint"] = "tcp://0.0.0.0:5580";
            j["out_zmq_bind"] = true;
            j["out_zmq_buffer_depth"] = 128;
            // Packing is schema-driven (removed from transport config
            // 2026-04-20) — no in/out_zmq_packing key here.  Pins that
            // the other ZMQ transport fields still parse without it.
            auto path = write_json(dir, "producer.json", j);
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.out_transport().transport, pylabhub::config::Transport::Zmq);
            EXPECT_EQ(cfg.out_transport().zmq_endpoint, "tcp://0.0.0.0:5580");
            EXPECT_TRUE(cfg.out_transport().zmq_bind);
            EXPECT_EQ(cfg.out_transport().zmq_buffer_depth, 128u);
        },
        "role_config::zmq_transport_valid",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int zmq_packing_key_rejected(const std::string &dir)
{
    // NEW (2026-04-20): pin that the removed `out_zmq_packing` /
    // `in_zmq_packing` keys are rejected by the strict-whitelist
    // parser.  Was a legal config key before; now a hard error.
    // Catches anyone trying to re-introduce the key without updating
    // the schema-driven packing propagation.
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["out_transport"] = "zmq";
            j["out_zmq_endpoint"] = "tcp://0.0.0.0:5580";
            j["out_zmq_packing"] = "packed";  // removed key
            auto path = write_json(dir, "producer.json", j);
            try
            {
                auto cfg = RoleConfig::load(path.string(), "producer");
                FAIL() << "out_zmq_packing must be rejected as unknown key";
            }
            catch (const std::exception &e)
            {
                EXPECT_NE(std::string(e.what()).find("out_zmq_packing"),
                          std::string::npos)
                    << "error message must name the offending key; got: "
                    << e.what();
            }
        },
        "role_config::zmq_packing_key_rejected",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Move semantics ──────────────────────────────────────────────────────────

int move_construct(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            RoleConfig moved(std::move(cfg));
            EXPECT_EQ(moved.identity().uid, "prod.test.u00000001");
        },
        "role_config::move_construct",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int move_assign(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg1 = RoleConfig::load(path.string(), "producer");
            auto path2 = write_json(dir, "consumer.json", minimal_consumer_json());
            auto cfg2 = RoleConfig::load(path2.string(), "consumer");
            cfg2 = std::move(cfg1);
            EXPECT_EQ(cfg2.identity().uid, "prod.test.u00000001");
            EXPECT_EQ(cfg2.role_tag(), "producer");
        },
        "role_config::move_assign",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Checksum ────────────────────────────────────────────────────────────────

int checksum_default_enforced(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.checksum().policy, pylabhub::hub::ChecksumPolicy::Enforced);
            EXPECT_TRUE(cfg.checksum().flexzone);
        },
        "role_config::checksum_default_enforced",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int checksum_explicit_manual(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["checksum"] = "manual";
            j["flexzone_checksum"] = false;
            auto path = write_json(dir, "producer.json", j);
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.checksum().policy, pylabhub::hub::ChecksumPolicy::Manual);
            EXPECT_FALSE(cfg.checksum().flexzone);
        },
        "role_config::checksum_explicit_manual",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int checksum_explicit_none(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_consumer_json();
            j["checksum"] = "none";
            auto path = write_json(dir, "consumer.json", j);
            auto cfg = RoleConfig::load(path.string(), "consumer");
            EXPECT_EQ(cfg.checksum().policy, pylabhub::hub::ChecksumPolicy::None);
        },
        "role_config::checksum_explicit_none",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int checksum_invalid_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["checksum"] = "turbo";
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::runtime_error &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("invalid 'checksum'"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr("turbo"));
            }
        },
        "role_config::checksum_invalid_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int checksum_null_default_enforced(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["checksum"] = nullptr;
            auto path = write_json(dir, "producer.json", j);
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.checksum().policy, pylabhub::hub::ChecksumPolicy::Enforced);
        },
        "role_config::checksum_null_default_enforced",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int unknown_key_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["unknown_bogus_key"] = 42;
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::runtime_error &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("unknown config key"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr("unknown_bogus_key"));
            }
        },
        "role_config::unknown_key_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int nested_unknown_key_throws(const std::string &dir)
{
    // Pins the nested-key whitelist on the 4 sub-parsers that have one.
    // Each sub-scope is probed with a single known-bad key; load() must
    // throw with the correct nested-path in the message.  Runs each
    // probe as an independent load() (distinct JSON file) so a failure
    // in one probe doesn't mask the others.
    return run_gtest_worker(
        [&]() {
            auto probe = [&](const char *tag,
                              const std::function<void(nlohmann::json &)> &mutator,
                              const std::string &expected_path)
            {
                SCOPED_TRACE(std::string("probe=") + tag);
                auto j = minimal_producer_json();
                mutator(j);
                auto path = write_json(dir, std::string(tag) + ".json", j);
                try {
                    RoleConfig::load(path.string(), "producer");
                    FAIL() << "expected RoleConfig::load to throw for " << tag;
                } catch (const std::runtime_error &e) {
                    EXPECT_THAT(e.what(), ::testing::HasSubstr("unknown config key"));
                    EXPECT_THAT(e.what(), ::testing::HasSubstr(expected_path))
                        << "expected message to contain '" << expected_path
                        << "', got: " << e.what();
                }
            };

            // script: `pahh` instead of `path` — the originating typo
            // scenario that surfaced the library bug.
            probe("script_typo",
                  [](nlohmann::json &j) { j["script"]["pahh"] = "/opt/x"; },
                  "script.pahh");

            // producer (role-tag block): unknown sibling of uid/name/log_level/auth.
            probe("producer_typo",
                  [](nlohmann::json &j) { j["producer"]["nme"] = "MyRole"; },
                  "producer.nme");

            // producer.auth: unknown sibling of keyfile.
            probe("auth_typo",
                  [](nlohmann::json &j) {
                      j["producer"]["auth"] = nlohmann::json::object();
                      j["producer"]["auth"]["keyfiile"] = "/etc/vault";
                  },
                  "producer.auth.keyfiile");

            // startup: unknown sibling of wait_for_roles.
            probe("startup_typo",
                  [](nlohmann::json &j) {
                      j["startup"]["wait_for_rolez"] =
                          nlohmann::json::array();
                  },
                  "startup.wait_for_rolez");

            // startup.wait_for_roles[i]: unknown sibling of uid/timeout_ms.
            probe("startup_entry_typo",
                  [](nlohmann::json &j) {
                      nlohmann::json entry = {
                          {"uid", "prod.other.u00000001"},
                          {"timout_ms", 1000}  // typo: timout vs timeout
                      };
                      j["startup"]["wait_for_roles"] =
                          nlohmann::json::array({entry});
                  },
                  "timout_ms");
        },
        "role_config::nested_unknown_key_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

// ── Logging ─────────────────────────────────────────────────────────────────

int logging_default_all_defaults(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto path = write_json(dir, "producer.json", minimal_producer_json());
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.logging().file_path, "");
            EXPECT_EQ(cfg.logging().max_size_bytes, 10ULL * 1024 * 1024);
            EXPECT_EQ(cfg.logging().max_backup_files, 5u);
            EXPECT_TRUE(cfg.logging().timestamped);
        },
        "role_config::logging_default_all_defaults",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_explicit_all_fields(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = {
                {"file_path",   "logs/custom.log"},
                {"max_size_mb", 50},
                {"backups",     10},
                {"timestamped", false},
            };
            auto path = write_json(dir, "producer.json", j);
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.logging().file_path, "logs/custom.log");
            EXPECT_EQ(cfg.logging().max_size_bytes, 50ULL * 1024 * 1024);
            EXPECT_EQ(cfg.logging().max_backup_files, 10u);
            EXPECT_FALSE(cfg.logging().timestamped);
        },
        "role_config::logging_explicit_all_fields",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_partial_mixes_defaults_and_explicit(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = {{"max_size_mb", 25}};
            auto path = write_json(dir, "producer.json", j);
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.logging().file_path, "");
            EXPECT_EQ(cfg.logging().max_size_bytes, 25ULL * 1024 * 1024);
            EXPECT_EQ(cfg.logging().max_backup_files, 5u);
            EXPECT_TRUE(cfg.logging().timestamped);
        },
        "role_config::logging_partial_mixes_defaults_and_explicit",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_zero_backups_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = {{"backups", 0}};
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::runtime_error &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("logging.backups"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr(">= 1"));
            }
        },
        "role_config::logging_zero_backups_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_backups_negative_one_keeps_all_files(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = {{"backups", -1}};
            auto path = write_json(dir, "producer.json", j);
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.logging().max_backup_files,
                      pylabhub::config::LoggingConfig::kKeepAllBackups);
        },
        "role_config::logging_backups_negative_one_keeps_all_files",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_negative_backups_other_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = {{"backups", -2}};
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::runtime_error &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("logging.backups"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr(">= 1"));
            }
        },
        "role_config::logging_negative_backups_other_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_zero_max_size_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = {{"max_size_mb", 0}};
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::runtime_error &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("logging.max_size_mb"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr("> 0"));
            }
        },
        "role_config::logging_zero_max_size_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_negative_max_size_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = {{"max_size_mb", -5}};
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::runtime_error &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("logging.max_size_mb"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr("> 0"));
            }
        },
        "role_config::logging_negative_max_size_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_unknown_sub_key_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = {
                {"max_size_mb", 10},
                {"bogus_field", "xyz"},
            };
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::runtime_error &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("unknown config key"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr("logging.bogus_field"));
            }
        },
        "role_config::logging_unknown_sub_key_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_not_object_throws(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = "not an object";
            auto path = write_json(dir, "producer.json", j);
            try {
                RoleConfig::load(path.string(), "producer");
                FAIL() << "expected RoleConfig::load to throw";
            } catch (const std::runtime_error &e) {
                EXPECT_THAT(e.what(), ::testing::HasSubstr("'logging'"));
                EXPECT_THAT(e.what(), ::testing::HasSubstr("must be a JSON object"));
            }
        },
        "role_config::logging_not_object_throws",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

int logging_fractional_max_size_accepted(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            auto j = minimal_producer_json();
            j["logging"] = {{"max_size_mb", 0.5}};
            auto path = write_json(dir, "producer.json", j);
            auto cfg = RoleConfig::load(path.string(), "producer");
            EXPECT_EQ(cfg.logging().max_size_bytes, 512ULL * 1024);
        },
        "role_config::logging_fractional_max_size_accepted",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

} // namespace role_config
} // namespace pylabhub::tests::worker

namespace
{

struct RoleConfigWorkerRegistrar
{
    RoleConfigWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "role_config")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::role_config;

                // file_not_found_throws() takes no dir argument.
                if (sc == "file_not_found_throws")
                    return file_not_found_throws();

                // All other scenarios require <dir> as argv[2].
                if (argc <= 2) {
                    fmt::print(stderr,
                               "role_config.{}: missing required <dir> arg\n", sc);
                    return 1;
                }
                const std::string dir = argv[2];

                if (sc == "load_producer_identity")
                    return load_producer_identity(dir);
                if (sc == "load_producer_timing")
                    return load_producer_timing(dir);
                if (sc == "load_producer_script")
                    return load_producer_script(dir);
                if (sc == "load_producer_out_channel")
                    return load_producer_out_channel(dir);
                if (sc == "load_producer_out_transport")
                    return load_producer_out_transport(dir);
                if (sc == "load_producer_out_validation")
                    return load_producer_out_validation(dir);
                if (sc == "load_producer_validation_stop_on_script_error")
                    return load_producer_validation_stop_on_script_error(dir);
                if (sc == "load_consumer_identity")
                    return load_consumer_identity(dir);
                if (sc == "load_consumer_in_channel")
                    return load_consumer_in_channel(dir);
                if (sc == "load_consumer_in_validation")
                    return load_consumer_in_validation(dir);
                if (sc == "load_consumer_default_timing")
                    return load_consumer_default_timing(dir);
                if (sc == "load_processor_dual_channels")
                    return load_processor_dual_channels(dir);
                if (sc == "load_processor_dual_transport")
                    return load_processor_dual_transport(dir);
                if (sc == "load_processor_dual_validation")
                    return load_processor_dual_validation(dir);
                if (sc == "role_data_producer_fields")
                    return role_data_producer_fields(dir);
                if (sc == "role_data_no_parser")
                    return role_data_no_parser(dir);
                if (sc == "role_data_wrong_type_cast_throws")
                    return role_data_wrong_type_cast_throws(dir);
                if (sc == "auth_default_empty")
                    return auth_default_empty(dir);
                if (sc == "load_keypair_no_keyfile_returns_false")
                    return load_keypair_no_keyfile_returns_false(dir);
                if (sc == "raw_json")
                    return raw_json(dir);
                if (sc == "role_tag")
                    return role_tag(dir);
                if (sc == "base_dir")
                    return base_dir(dir);
                if (sc == "load_from_directory")
                    return load_from_directory(dir);
                if (sc == "invalid_script_type_throws")
                    return invalid_script_type_throws(dir);
                if (sc == "zmq_transport_missing_endpoint_throws")
                    return zmq_transport_missing_endpoint_throws(dir);
                if (sc == "zmq_transport_valid")
                    return zmq_transport_valid(dir);
                if (sc == "move_construct")
                    return move_construct(dir);
                if (sc == "move_assign")
                    return move_assign(dir);
                if (sc == "checksum_default_enforced")
                    return checksum_default_enforced(dir);
                if (sc == "checksum_explicit_manual")
                    return checksum_explicit_manual(dir);
                if (sc == "checksum_explicit_none")
                    return checksum_explicit_none(dir);
                if (sc == "checksum_invalid_throws")
                    return checksum_invalid_throws(dir);
                if (sc == "checksum_null_default_enforced")
                    return checksum_null_default_enforced(dir);
                if (sc == "unknown_key_throws")
                    return unknown_key_throws(dir);
                if (sc == "nested_unknown_key_throws")
                    return nested_unknown_key_throws(dir);
                if (sc == "logging_default_all_defaults")
                    return logging_default_all_defaults(dir);
                if (sc == "logging_explicit_all_fields")
                    return logging_explicit_all_fields(dir);
                if (sc == "logging_partial_mixes_defaults_and_explicit")
                    return logging_partial_mixes_defaults_and_explicit(dir);
                if (sc == "logging_zero_backups_throws")
                    return logging_zero_backups_throws(dir);
                if (sc == "logging_backups_negative_one_keeps_all_files")
                    return logging_backups_negative_one_keeps_all_files(dir);
                if (sc == "logging_negative_backups_other_throws")
                    return logging_negative_backups_other_throws(dir);
                if (sc == "logging_zero_max_size_throws")
                    return logging_zero_max_size_throws(dir);
                if (sc == "logging_negative_max_size_throws")
                    return logging_negative_max_size_throws(dir);
                if (sc == "logging_unknown_sub_key_throws")
                    return logging_unknown_sub_key_throws(dir);
                if (sc == "logging_not_object_throws")
                    return logging_not_object_throws(dir);
                if (sc == "logging_fractional_max_size_accepted")
                    return logging_fractional_max_size_accepted(dir);

                fmt::print(stderr,
                           "[role_config] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static RoleConfigWorkerRegistrar g_role_config_registrar;

} // namespace
