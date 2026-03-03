/**
 * @file test_processor_config.cpp
 * @brief Unit tests for ProcessorConfig JSON parsing (no lifecycle, no Python).
 *
 * processor_config.cpp is compiled directly into this test binary so we can
 * exercise ProcessorConfig::from_json_file() and from_directory() without the
 * pybind11 / scripting dependencies that are private to the processor executable.
 *
 * Pattern 1 (PureApiTest) — no module init, no lifecycle.
 */

#include "processor_config.hpp"

#include "test_patterns.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

// ── Temp-dir helper ──────────────────────────────────────────────────────────

static fs::path unique_temp_dir(const std::string &prefix)
{
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1);
    fs::path dir = fs::temp_directory_path()
                   / ("plh_proccfg_" + prefix + "_" + std::to_string(id));
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path &path, const std::string &content)
{
    std::ofstream f(path);
    f << content;
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class ProcessorConfigTest : public pylabhub::tests::PureApiTest {};

// ── Tests ────────────────────────────────────────────────────────────────────

TEST_F(ProcessorConfigTest, FromJsonFile_Basic)
{
    const auto tmp      = unique_temp_dir("basic");
    const auto cfg_path = tmp / "processor.json";

    write_file(cfg_path, R"({
        "processor": {
            "uid":       "PROC-TESTSCL-AABBCCDD",
            "name":      "TestScaler",
            "log_level": "debug"
        },
        "in_channel":  "lab.input.channel",
        "out_channel": "lab.output.channel",
        "overflow_policy": "drop",
        "timeout_ms":  3000,
        "heartbeat_interval_ms": 500,
        "shm": {
            "in":  { "enabled": true,  "secret": 11111 },
            "out": { "enabled": true,  "secret": 22222, "slot_count": 8 }
        },
        "script": { "path": "./script", "type": "python" },
        "validation": { "update_checksum": true, "stop_on_script_error": true }
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string());

    EXPECT_EQ(cfg.processor_uid,    "PROC-TESTSCL-AABBCCDD");
    EXPECT_EQ(cfg.processor_name,   "TestScaler");
    EXPECT_EQ(cfg.log_level,        "debug");
    EXPECT_EQ(cfg.in_channel,       "lab.input.channel");
    EXPECT_EQ(cfg.out_channel,      "lab.output.channel");
    EXPECT_EQ(cfg.overflow_policy,  pylabhub::processor::OverflowPolicy::Drop);
    EXPECT_EQ(cfg.timeout_ms,       3000);
    EXPECT_EQ(cfg.heartbeat_interval_ms, 500);
    EXPECT_TRUE(cfg.in_shm_enabled);
    EXPECT_EQ(cfg.in_shm_secret,    uint64_t{11111});
    EXPECT_TRUE(cfg.out_shm_enabled);
    EXPECT_EQ(cfg.out_shm_secret,   uint64_t{22222});
    EXPECT_EQ(cfg.out_shm_slot_count, uint32_t{8});
    EXPECT_EQ(cfg.script_path,      "./script");
    EXPECT_EQ(cfg.script_type,      "python");
    EXPECT_TRUE(cfg.update_checksum);
    EXPECT_TRUE(cfg.stop_on_script_error);

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, FromJsonFile_UidAutoGen)
{
    const auto tmp      = unique_temp_dir("uid");
    const auto cfg_path = tmp / "processor.json";

    // No "uid" field — should be auto-generated with PROC- prefix
    write_file(cfg_path, R"({
        "processor": { "name": "AutoProc" },
        "in_channel":  "lab.auto.in",
        "out_channel": "lab.auto.out"
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string());

    EXPECT_FALSE(cfg.processor_uid.empty());
    EXPECT_EQ(cfg.processor_uid.rfind("PROC-", 0), 0u)
        << "Auto-generated uid must start with PROC-, got: " << cfg.processor_uid;

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, FromJsonFile_SchemaFields)
{
    const auto tmp      = unique_temp_dir("schema");
    const auto cfg_path = tmp / "processor.json";

    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-SCHTEST-00000001", "name": "SchTest" },
        "in_channel":  "lab.schema.in",
        "out_channel": "lab.schema.out",
        "in_slot_schema": {
            "fields": [
                {"name": "raw_value", "type": "float32"},
                {"name": "timestamp", "type": "uint64"}
            ]
        },
        "out_slot_schema": {
            "fields": [
                {"name": "scaled_value", "type": "float64"}
            ]
        },
        "flexzone_schema": {
            "fields": [
                {"name": "meta", "type": "uint8", "count": 64}
            ]
        }
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string());

    ASSERT_FALSE(cfg.in_slot_schema_json.is_null());
    ASSERT_FALSE(cfg.out_slot_schema_json.is_null());
    ASSERT_FALSE(cfg.flexzone_schema_json.is_null());

    const auto &in_fields = cfg.in_slot_schema_json["fields"];
    ASSERT_EQ(in_fields.size(), 2u);
    EXPECT_EQ(in_fields[0]["name"].get<std::string>(), "raw_value");
    EXPECT_EQ(in_fields[0]["type"].get<std::string>(), "float32");
    EXPECT_EQ(in_fields[1]["name"].get<std::string>(), "timestamp");
    EXPECT_EQ(in_fields[1]["type"].get<std::string>(), "uint64");

    const auto &out_fields = cfg.out_slot_schema_json["fields"];
    ASSERT_EQ(out_fields.size(), 1u);
    EXPECT_EQ(out_fields[0]["name"].get<std::string>(), "scaled_value");
    EXPECT_EQ(out_fields[0]["type"].get<std::string>(), "float64");

    const auto &fz_fields = cfg.flexzone_schema_json["fields"];
    ASSERT_EQ(fz_fields.size(), 1u);
    EXPECT_EQ(fz_fields[0]["name"].get<std::string>(), "meta");
    EXPECT_EQ(fz_fields[0]["count"].get<int>(), 64);

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, FromJsonFile_MissingInChannel)
{
    const auto tmp      = unique_temp_dir("noin");
    const auto cfg_path = tmp / "processor.json";

    // Missing in_channel — should throw
    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-NOIN-00000001", "name": "NoIn" },
        "out_channel": "lab.output.test"
    })");

    EXPECT_THROW(
        pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string()),
        std::runtime_error);

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, FromJsonFile_MissingOutChannel)
{
    const auto tmp      = unique_temp_dir("noout");
    const auto cfg_path = tmp / "processor.json";

    // Missing out_channel — should throw
    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-NOOUT-00000001", "name": "NoOut" },
        "in_channel": "lab.input.test"
    })");

    EXPECT_THROW(
        pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string()),
        std::runtime_error);

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, FromJsonFile_MalformedJson)
{
    const auto tmp      = unique_temp_dir("badjs");
    const auto cfg_path = tmp / "processor.json";

    write_file(cfg_path, R"({ "processor": { broken json )");

    EXPECT_THROW(
        pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string()),
        std::runtime_error);

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, FromJsonFile_FileNotFound)
{
    EXPECT_THROW(
        pylabhub::processor::ProcessorConfig::from_json_file(
            "/no/such/path/does/not/exist/processor.json"),
        std::runtime_error);
}

TEST_F(ProcessorConfigTest, FromDirectory_Basic)
{
    const auto tmp      = unique_temp_dir("dir");
    const auto cfg_path = tmp / "processor.json";

    // No hub_dir: from_directory() skips hub.json lookup.
    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-DIRTEST-00000001", "name": "DirTest" },
        "in_channel":  "lab.dir.in",
        "out_channel": "lab.dir.out",
        "script": { "path": "./script", "type": "python" }
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_directory(tmp.string());

    EXPECT_EQ(cfg.processor_uid, "PROC-DIRTEST-00000001");
    EXPECT_EQ(cfg.in_channel,    "lab.dir.in");
    EXPECT_EQ(cfg.out_channel,   "lab.dir.out");

    // from_directory() resolves relative script_path to absolute
    EXPECT_TRUE(fs::path(cfg.script_path).is_absolute())
        << "script_path should be absolute after from_directory(), got: " << cfg.script_path;

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, OverflowPolicy_DefaultBlock)
{
    const auto tmp      = unique_temp_dir("defpol");
    const auto cfg_path = tmp / "processor.json";

    // No "overflow_policy" field — should default to "block"
    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-DEFPOL-00000001", "name": "DefPol" },
        "in_channel":  "lab.policy.in",
        "out_channel": "lab.policy.out"
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string());

    EXPECT_EQ(cfg.overflow_policy, pylabhub::processor::OverflowPolicy::Block);
    EXPECT_EQ(cfg.timeout_ms, -1);               // default
    EXPECT_FALSE(cfg.stop_on_script_error);       // default false
    EXPECT_TRUE(cfg.update_checksum);             // default true

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, OverflowPolicy_InvalidThrows)
{
    const auto tmp      = unique_temp_dir("badpol");
    const auto cfg_path = tmp / "processor.json";

    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-BADPOL-00000001", "name": "BadPol" },
        "in_channel":  "lab.badpol.in",
        "out_channel": "lab.badpol.out",
        "overflow_policy": "invalid_policy"
    })");

    EXPECT_THROW(
        pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string()),
        std::runtime_error);

    fs::remove_all(tmp);
}

// ── Named schema string fields (HEP-CORE-0016 Phase 5) ──────────────────────

TEST_F(ProcessorConfigTest, SchemaStringField_StoredAsString)
{
    const auto tmp      = unique_temp_dir("strsch");
    const auto cfg_path = tmp / "processor.json";

    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-STRSCH-00000001", "name": "StrSch" },
        "in_channel":  "lab.strsch.in",
        "out_channel": "lab.strsch.out",
        "in_slot_schema":  "lab.sensors.temperature.raw@1",
        "out_slot_schema": "lab.sensors.scaled@2",
        "flexzone_schema": "lab.sensors.meta@1"
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string());

    ASSERT_TRUE(cfg.in_slot_schema_json.is_string());
    EXPECT_EQ(cfg.in_slot_schema_json.get<std::string>(), "lab.sensors.temperature.raw@1");

    ASSERT_TRUE(cfg.out_slot_schema_json.is_string());
    EXPECT_EQ(cfg.out_slot_schema_json.get<std::string>(), "lab.sensors.scaled@2");

    ASSERT_TRUE(cfg.flexzone_schema_json.is_string());
    EXPECT_EQ(cfg.flexzone_schema_json.get<std::string>(), "lab.sensors.meta@1");

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, SchemaObjectField_StillWorks)
{
    const auto tmp      = unique_temp_dir("objsch");
    const auto cfg_path = tmp / "processor.json";

    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-OBJSCH-00000001", "name": "ObjSch" },
        "in_channel":  "lab.objsch.in",
        "out_channel": "lab.objsch.out",
        "in_slot_schema": {
            "fields": [{"name": "x", "type": "float32"}]
        }
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string());

    ASSERT_TRUE(cfg.in_slot_schema_json.is_object());
    ASSERT_TRUE(cfg.in_slot_schema_json.contains("fields"));
    EXPECT_EQ(cfg.in_slot_schema_json["fields"].size(), 1u);

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, SchemaNullField_StillWorks)
{
    const auto tmp      = unique_temp_dir("nullsch");
    const auto cfg_path = tmp / "processor.json";

    // No schema fields at all — all default to null
    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-NULLSCH-00000001", "name": "NullSch" },
        "in_channel":  "lab.nullsch.in",
        "out_channel": "lab.nullsch.out"
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string());

    EXPECT_TRUE(cfg.in_slot_schema_json.is_null());
    EXPECT_TRUE(cfg.out_slot_schema_json.is_null());
    EXPECT_TRUE(cfg.flexzone_schema_json.is_null());

    fs::remove_all(tmp);
}

// ── Dual-broker config tests (HEP-CORE-0015 Phase 2) ────────────────────────

TEST_F(ProcessorConfigTest, DualBroker_BothPresent)
{
    const auto tmp      = unique_temp_dir("dualb");
    const auto cfg_path = tmp / "processor.json";

    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-DUALB-00000001", "name": "DualB" },
        "in_channel":  "lab.dual.in",
        "out_channel": "lab.dual.out",
        "broker": "tcp://default:5570",
        "broker_pubkey": "DEFAULTPUBKEY000000000000000000000000000",
        "in_broker":  "tcp://input-hub:5570",
        "out_broker": "tcp://output-hub:5570",
        "in_broker_pubkey":  "INPUTPUBKEY0000000000000000000000000000000",
        "out_broker_pubkey": "OUTPUTPUBKEY000000000000000000000000000000"
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string());

    // Per-direction fields parsed correctly
    EXPECT_EQ(cfg.in_broker,         "tcp://input-hub:5570");
    EXPECT_EQ(cfg.out_broker,        "tcp://output-hub:5570");
    EXPECT_EQ(cfg.in_broker_pubkey,  "INPUTPUBKEY0000000000000000000000000000000");
    EXPECT_EQ(cfg.out_broker_pubkey, "OUTPUTPUBKEY000000000000000000000000000000");

    // Resolvers return per-direction values
    EXPECT_EQ(cfg.resolved_in_broker(),         "tcp://input-hub:5570");
    EXPECT_EQ(cfg.resolved_out_broker(),        "tcp://output-hub:5570");
    EXPECT_EQ(cfg.resolved_in_broker_pubkey(),  "INPUTPUBKEY0000000000000000000000000000000");
    EXPECT_EQ(cfg.resolved_out_broker_pubkey(), "OUTPUTPUBKEY000000000000000000000000000000");

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, DualBroker_FallbackToSingle)
{
    const auto tmp      = unique_temp_dir("dualfb");
    const auto cfg_path = tmp / "processor.json";

    // No per-direction fields — resolvers should fall back to global broker
    write_file(cfg_path, R"({
        "processor": { "uid": "PROC-DUALFB-00000001", "name": "DualFB" },
        "in_channel":  "lab.dualfb.in",
        "out_channel": "lab.dualfb.out",
        "broker": "tcp://single:5570",
        "broker_pubkey": "SINGLEPUBKEY000000000000000000000000000000"
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_json_file(cfg_path.string());

    // Per-direction fields are empty
    EXPECT_TRUE(cfg.in_broker.empty());
    EXPECT_TRUE(cfg.out_broker.empty());

    // Resolvers fall back to global
    EXPECT_EQ(cfg.resolved_in_broker(),         "tcp://single:5570");
    EXPECT_EQ(cfg.resolved_out_broker(),        "tcp://single:5570");
    EXPECT_EQ(cfg.resolved_in_broker_pubkey(),  "SINGLEPUBKEY000000000000000000000000000000");
    EXPECT_EQ(cfg.resolved_out_broker_pubkey(), "SINGLEPUBKEY000000000000000000000000000000");

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, DualBroker_InHubDir)
{
    const auto tmp = unique_temp_dir("dualihd");

    // Create input hub directory with hub.json + hub.pubkey
    const auto in_hub = tmp / "input_hub";
    fs::create_directories(in_hub);
    write_file(in_hub / "hub.json", R"({
        "hub": { "broker_endpoint": "tcp://input-hub-from-dir:5570" }
    })");
    write_file(in_hub / "hub.pubkey", "INPUTDIRPUBKEY0000000000000000000000000000");

    write_file(tmp / "processor.json", R"({
        "processor": { "uid": "PROC-DUALIHD-00000001", "name": "DualIHD" },
        "in_channel":  "lab.dualihd.in",
        "out_channel": "lab.dualihd.out",
        "in_hub_dir": "input_hub"
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_directory(tmp.string());

    // in_hub_dir resolved → in_broker and in_broker_pubkey populated
    EXPECT_EQ(cfg.in_broker,        "tcp://input-hub-from-dir:5570");
    EXPECT_EQ(cfg.in_broker_pubkey, "INPUTDIRPUBKEY0000000000000000000000000000");

    // Resolvers use per-direction
    EXPECT_EQ(cfg.resolved_in_broker(), "tcp://input-hub-from-dir:5570");

    // Output side falls back to global
    EXPECT_EQ(cfg.resolved_out_broker(), cfg.broker);

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, DualBroker_OutHubDir)
{
    const auto tmp = unique_temp_dir("dualohd");

    // Create output hub directory
    const auto out_hub = tmp / "output_hub";
    fs::create_directories(out_hub);
    write_file(out_hub / "hub.json", R"({
        "hub": { "broker_endpoint": "tcp://output-hub-from-dir:5570" }
    })");
    write_file(out_hub / "hub.pubkey", "OUTPUTDIRPUBKEY000000000000000000000000000");

    write_file(tmp / "processor.json", R"({
        "processor": { "uid": "PROC-DUALOHD-00000001", "name": "DualOHD" },
        "in_channel":  "lab.dualohd.in",
        "out_channel": "lab.dualohd.out",
        "out_hub_dir": "output_hub"
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_directory(tmp.string());

    EXPECT_EQ(cfg.out_broker,        "tcp://output-hub-from-dir:5570");
    EXPECT_EQ(cfg.out_broker_pubkey, "OUTPUTDIRPUBKEY000000000000000000000000000");
    EXPECT_EQ(cfg.resolved_out_broker(), "tcp://output-hub-from-dir:5570");

    // Input side falls back to global
    EXPECT_EQ(cfg.resolved_in_broker(), cfg.broker);

    fs::remove_all(tmp);
}

TEST_F(ProcessorConfigTest, DualBroker_MixedConfig)
{
    const auto tmp = unique_temp_dir("dualmix");

    // Input: explicit in_broker field
    // Output: out_hub_dir → resolved from directory
    const auto out_hub = tmp / "out_hub";
    fs::create_directories(out_hub);
    write_file(out_hub / "hub.json", R"({
        "hub": { "broker_endpoint": "tcp://outhub:5570" }
    })");

    write_file(tmp / "processor.json", R"({
        "processor": { "uid": "PROC-DUALMX-00000001", "name": "DualMx" },
        "in_channel":  "lab.dualmx.in",
        "out_channel": "lab.dualmx.out",
        "broker": "tcp://global:5570",
        "in_broker": "tcp://explicit-input:5570",
        "out_hub_dir": "out_hub"
    })");

    const auto cfg = pylabhub::processor::ProcessorConfig::from_directory(tmp.string());

    // Input: explicit in_broker takes precedence
    EXPECT_EQ(cfg.resolved_in_broker(), "tcp://explicit-input:5570");

    // Output: from out_hub_dir
    EXPECT_EQ(cfg.resolved_out_broker(), "tcp://outhub:5570");

    // Global broker is still "tcp://global:5570"
    EXPECT_EQ(cfg.broker, "tcp://global:5570");

    fs::remove_all(tmp);
}
