/**
 * @file test_role_config.cpp
 * @brief L2 unit tests for config::RoleConfig — unified config class.
 *
 * Tests the factory methods, common/directional accessors, role-specific
 * data, JsonConfig backend integration, and validation.
 */

#include "utils/config/role_config.hpp"
#include "plh_datahub.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <any>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using pylabhub::config::RoleConfig;
using namespace pylabhub::utils;

// ============================================================================
// Test fixtures
// ============================================================================

class RoleConfigTest : public ::testing::Test
{
  protected:
    static std::unique_ptr<LifecycleGuard> s_lifecycle;

    static void SetUpTestSuite()
    {
        s_lifecycle = std::make_unique<LifecycleGuard>(
            MakeModDefList(JsonConfig::GetLifecycleModule(),
                           FileLock::GetLifecycleModule(),
                           Logger::GetLifecycleModule()),
            std::source_location::current());
    }

    static void TearDownTestSuite()
    {
        s_lifecycle.reset();
    }

    fs::path tmp_dir_;

    void SetUp() override
    {
        // Use unique dir per test to avoid parallel test races.
        tmp_dir_ = fs::temp_directory_path() /
                   ("pylabhub_rcfg_" + std::to_string(::getpid()));
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    /// Write a JSON file and return its path.
    fs::path write_json(const std::string &filename, const nlohmann::json &j)
    {
        const auto path = tmp_dir_ / filename;
        std::ofstream ofs(path);
        ofs << j.dump(2);
        return path;
    }

    /// Minimal producer JSON with new in_/out_ naming.
    nlohmann::json minimal_producer_json()
    {
        return {
            {"producer", {{"uid", "PROD-TEST-00000001"}, {"name", "TestProd"}}},
            {"out_hub_dir", ""},
            {"out_channel", "test.channel"},
            {"out_transport", "shm"},
            {"out_shm_enabled", true},
            {"out_shm_slot_count", 8},
            {"out_update_checksum", true},
            {"out_slot_schema", {{"fields", {{{"name", "value"}, {"type", "float32"}}}}}},
            {"target_period_ms", 50.0},
            {"script", {{"type", "python"}, {"path", "."}}},
        };
    }

    /// Minimal consumer JSON.
    nlohmann::json minimal_consumer_json()
    {
        return {
            {"consumer", {{"uid", "CONS-TEST-00000002"}, {"name", "TestCons"}}},
            {"in_hub_dir", ""},
            {"in_channel", "test.channel"},
            {"in_transport", "shm"},
            {"in_verify_checksum", true},
            {"script", {{"type", "lua"}, {"path", "."}}},
        };
    }

    /// Minimal processor JSON (dual-hub).
    nlohmann::json minimal_processor_json()
    {
        return {
            {"processor", {{"uid", "PROC-TEST-00000003"}, {"name", "TestProc"}}},
            {"in_hub_dir", ""},
            {"out_hub_dir", ""},
            {"in_channel", "raw.data"},
            {"out_channel", "processed.data"},
            {"in_transport", "shm"},
            {"out_transport", "zmq"},
            {"out_zmq_endpoint", "tcp://0.0.0.0:5599"},
            {"out_zmq_bind", true},
            {"out_slot_schema", {{"fields", {{{"name", "result"}, {"type", "float64"}}}}}},
            {"out_update_checksum", true},
            {"in_verify_checksum", false},
            {"script", {{"type", "python"}, {"path", "."}}},
        };
    }
};

std::unique_ptr<LifecycleGuard> RoleConfigTest::s_lifecycle;

// ============================================================================
// Producer role-specific fields
// ============================================================================

struct TestProducerFields
{
    nlohmann::json out_slot_schema;
};

static std::any parse_test_producer(const nlohmann::json &j,
                                     const RoleConfig & /*cfg*/)
{
    TestProducerFields pf;
    pf.out_slot_schema = j.value("out_slot_schema", nlohmann::json{});
    return pf;
}

// ============================================================================
// Tests: Factory + common accessors
// ============================================================================

TEST_F(RoleConfigTest, LoadProducer_Identity)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_EQ(cfg.identity().uid, "PROD-TEST-00000001");
    EXPECT_EQ(cfg.identity().name, "TestProd");
    EXPECT_EQ(cfg.identity().log_level, "info");
}

TEST_F(RoleConfigTest, LoadProducer_Timing)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    // 50ms = 50000us
    EXPECT_DOUBLE_EQ(cfg.timing().period_us, 50000.0);
}

TEST_F(RoleConfigTest, LoadProducer_Script)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_EQ(cfg.script().type, "python");
    EXPECT_TRUE(cfg.script().type_explicit);
}

TEST_F(RoleConfigTest, LoadProducer_OutChannel)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_EQ(cfg.out_channel(), "test.channel");
    EXPECT_TRUE(cfg.in_channel().empty()); // producer has no input
}

TEST_F(RoleConfigTest, LoadProducer_OutTransport)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_EQ(cfg.out_transport().transport, pylabhub::config::Transport::Shm);
    EXPECT_EQ(cfg.out_shm().slot_count, 8u);
    EXPECT_TRUE(cfg.out_shm().enabled);
}

TEST_F(RoleConfigTest, LoadProducer_OutValidation)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_TRUE(cfg.out_validation().update_checksum);
}

TEST_F(RoleConfigTest, LoadProducer_Validation_StopOnScriptError)
{
    auto j = minimal_producer_json();
    j["stop_on_script_error"] = true;
    auto path = write_json("producer.json", j);
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_TRUE(cfg.validation().stop_on_script_error);
}

// ============================================================================
// Tests: Consumer
// ============================================================================

TEST_F(RoleConfigTest, LoadConsumer_Identity)
{
    auto path = write_json("consumer.json", minimal_consumer_json());
    auto cfg = RoleConfig::load(path.string(), "consumer");

    EXPECT_EQ(cfg.identity().uid, "CONS-TEST-00000002");
    EXPECT_EQ(cfg.script().type, "lua");
}

TEST_F(RoleConfigTest, LoadConsumer_InChannel)
{
    auto path = write_json("consumer.json", minimal_consumer_json());
    auto cfg = RoleConfig::load(path.string(), "consumer");

    EXPECT_EQ(cfg.in_channel(), "test.channel");
    EXPECT_TRUE(cfg.out_channel().empty()); // consumer has no output
}

TEST_F(RoleConfigTest, LoadConsumer_InValidation)
{
    auto path = write_json("consumer.json", minimal_consumer_json());
    auto cfg = RoleConfig::load(path.string(), "consumer");

    EXPECT_TRUE(cfg.in_validation().verify_checksum);
}

TEST_F(RoleConfigTest, LoadConsumer_DefaultTiming)
{
    auto path = write_json("consumer.json", minimal_consumer_json());
    auto cfg = RoleConfig::load(path.string(), "consumer");

    // Consumer default: period=0 (demand-driven), MaxRate
    EXPECT_DOUBLE_EQ(cfg.timing().period_us, 0.0);
    EXPECT_EQ(cfg.timing().loop_timing, pylabhub::LoopTimingPolicy::MaxRate);
}

// ============================================================================
// Tests: Processor (dual-hub, dual-transport)
// ============================================================================

TEST_F(RoleConfigTest, LoadProcessor_DualChannels)
{
    auto path = write_json("processor.json", minimal_processor_json());
    auto cfg = RoleConfig::load(path.string(), "processor");

    EXPECT_EQ(cfg.in_channel(), "raw.data");
    EXPECT_EQ(cfg.out_channel(), "processed.data");
}

TEST_F(RoleConfigTest, LoadProcessor_DualTransport)
{
    auto path = write_json("processor.json", minimal_processor_json());
    auto cfg = RoleConfig::load(path.string(), "processor");

    EXPECT_EQ(cfg.in_transport().transport, pylabhub::config::Transport::Shm);
    EXPECT_EQ(cfg.out_transport().transport, pylabhub::config::Transport::Zmq);
    EXPECT_EQ(cfg.out_transport().zmq_endpoint, "tcp://0.0.0.0:5599");
    EXPECT_TRUE(cfg.out_transport().zmq_bind);
}

TEST_F(RoleConfigTest, LoadProcessor_DualValidation)
{
    auto path = write_json("processor.json", minimal_processor_json());
    auto cfg = RoleConfig::load(path.string(), "processor");

    EXPECT_TRUE(cfg.out_validation().update_checksum);
    EXPECT_FALSE(cfg.in_validation().verify_checksum);
}

// ============================================================================
// Tests: Role-specific data (std::any)
// ============================================================================

TEST_F(RoleConfigTest, RoleData_ProducerFields)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer", parse_test_producer);

    EXPECT_TRUE(cfg.has_role_data());
    const auto &pf = cfg.role_data<TestProducerFields>();
    EXPECT_FALSE(pf.out_slot_schema.empty());
    EXPECT_TRUE(pf.out_slot_schema.contains("fields"));
}

TEST_F(RoleConfigTest, RoleData_NoParser)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_FALSE(cfg.has_role_data());
}

TEST_F(RoleConfigTest, RoleData_WrongTypeCast_Throws)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer", parse_test_producer);

    // Casting to wrong type should throw std::bad_any_cast.
    struct WrongType {};
    EXPECT_THROW(cfg.role_data<WrongType>(), std::bad_any_cast);
}

// ============================================================================
// Tests: Mutable auth
// ============================================================================

TEST_F(RoleConfigTest, Auth_DefaultEmpty)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_TRUE(cfg.auth().keyfile.empty());
    EXPECT_TRUE(cfg.auth().client_pubkey.empty());
    EXPECT_TRUE(cfg.auth().client_seckey.empty());
}

TEST_F(RoleConfigTest, LoadKeypair_NoKeyfile_ReturnsFalse)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_FALSE(cfg.load_keypair("dummy-password"));
}

// ============================================================================
// Tests: Raw JSON
// ============================================================================

TEST_F(RoleConfigTest, RawJson)
{
    auto j = minimal_producer_json();
    auto path = write_json("producer.json", j);
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_TRUE(cfg.raw().contains("producer"));
    EXPECT_TRUE(cfg.raw().contains("out_channel"));
    EXPECT_EQ(cfg.raw()["out_channel"], "test.channel");
}

// ============================================================================
// Tests: Metadata
// ============================================================================

TEST_F(RoleConfigTest, RoleTag)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_EQ(cfg.role_tag(), "producer");
}

TEST_F(RoleConfigTest, BaseDir)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    EXPECT_EQ(cfg.base_dir(), tmp_dir_);
}

// ============================================================================
// Tests: load_from_directory
// ============================================================================

TEST_F(RoleConfigTest, LoadFromDirectory)
{
    write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load_from_directory(tmp_dir_.string(), "producer");

    EXPECT_EQ(cfg.identity().uid, "PROD-TEST-00000001");
    EXPECT_EQ(cfg.out_channel(), "test.channel");
}

// ============================================================================
// Tests: Validation errors
// ============================================================================

TEST_F(RoleConfigTest, FileNotFound_Throws)
{
    EXPECT_THROW(
        RoleConfig::load("/nonexistent/path.json", "producer"),
        std::runtime_error);
}

TEST_F(RoleConfigTest, InvalidScriptType_Throws)
{
    auto j = minimal_producer_json();
    j["script"]["type"] = "ruby";
    auto path = write_json("producer.json", j);

    EXPECT_THROW(
        RoleConfig::load(path.string(), "producer"),
        std::exception);
}

TEST_F(RoleConfigTest, ZmqTransport_MissingEndpoint_Throws)
{
    auto j = minimal_producer_json();
    j["out_transport"] = "zmq";
    // No out_zmq_endpoint provided.
    auto path = write_json("producer.json", j);

    EXPECT_THROW(
        RoleConfig::load(path.string(), "producer"),
        std::exception);
}

TEST_F(RoleConfigTest, ZmqTransport_Valid)
{
    auto j = minimal_producer_json();
    j["out_transport"] = "zmq";
    j["out_zmq_endpoint"] = "tcp://0.0.0.0:5580";
    j["out_zmq_bind"] = true;
    j["out_zmq_buffer_depth"] = 128;
    j["out_zmq_packing"] = "packed";
    auto path = write_json("producer.json", j);

    auto cfg = RoleConfig::load(path.string(), "producer");
    EXPECT_EQ(cfg.out_transport().transport, pylabhub::config::Transport::Zmq);
    EXPECT_EQ(cfg.out_transport().zmq_endpoint, "tcp://0.0.0.0:5580");
    EXPECT_TRUE(cfg.out_transport().zmq_bind);
    EXPECT_EQ(cfg.out_transport().zmq_buffer_depth, 128u);
    EXPECT_EQ(cfg.out_transport().zmq_packing, "packed");
}

// ============================================================================
// Tests: Move semantics
// ============================================================================

TEST_F(RoleConfigTest, MoveConstruct)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg = RoleConfig::load(path.string(), "producer");

    RoleConfig moved(std::move(cfg));
    EXPECT_EQ(moved.identity().uid, "PROD-TEST-00000001");
}

TEST_F(RoleConfigTest, MoveAssign)
{
    auto path = write_json("producer.json", minimal_producer_json());
    auto cfg1 = RoleConfig::load(path.string(), "producer");

    auto j2 = minimal_consumer_json();
    auto path2 = write_json("consumer.json", j2);
    auto cfg2 = RoleConfig::load(path2.string(), "consumer");

    cfg2 = std::move(cfg1);
    EXPECT_EQ(cfg2.identity().uid, "PROD-TEST-00000001");
    EXPECT_EQ(cfg2.role_tag(), "producer");
}
