/**
 * @file test_producer_config.cpp
 * @brief Unit tests for ProducerConfig JSON parsing (no lifecycle, no Python).
 *
 * producer_config.cpp is compiled directly into this test binary so we can
 * exercise ProducerConfig::from_json_file() and from_directory() without the
 * pybind11 / scripting dependencies that are private to the producer executable.
 *
 * Pattern 1 (PureApiTest) — no module init, no lifecycle.
 */

#include "producer_config.hpp"

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
                   / ("plh_prodcfg_" + prefix + "_" + std::to_string(id));
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path &path, const std::string &content)
{
    std::ofstream f(path);
    f << content;
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class ProducerConfigTest : public pylabhub::tests::PureApiTest {};

// ── Tests ────────────────────────────────────────────────────────────────────

TEST_F(ProducerConfigTest, FromJsonFile_Basic)
{
    const auto tmp      = unique_temp_dir("basic");
    const auto cfg_path = tmp / "producer.json";

    write_file(cfg_path, R"({
        "producer": {
            "uid":       "PROD-TESTSENS-AABBCCDD",
            "name":      "TestSensor",
            "log_level": "debug"
        },
        "channel":     "lab.test.channel",
        "interval_ms": 200,
        "timeout_ms":  3000,
        "shm": { "enabled": true, "secret": 12345, "slot_count": 16 },
        "script": { "path": "./script", "type": "python" },
        "validation": { "update_checksum": true, "stop_on_script_error": true }
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    EXPECT_EQ(cfg.producer_uid,    "PROD-TESTSENS-AABBCCDD");
    EXPECT_EQ(cfg.producer_name,   "TestSensor");
    EXPECT_EQ(cfg.log_level,       "debug");
    EXPECT_EQ(cfg.channel,         "lab.test.channel");
    EXPECT_EQ(cfg.interval_ms,     200);
    EXPECT_EQ(cfg.timeout_ms,      3000);
    EXPECT_TRUE(cfg.shm_enabled);
    EXPECT_EQ(cfg.shm_secret,      uint64_t{12345});
    EXPECT_EQ(cfg.shm_slot_count,  uint32_t{16});
    EXPECT_EQ(cfg.script_path,     "./script");
    EXPECT_EQ(cfg.script_type,     "python");
    EXPECT_TRUE(cfg.update_checksum);
    EXPECT_TRUE(cfg.stop_on_script_error);

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, FromJsonFile_UidAutoGen)
{
    const auto tmp      = unique_temp_dir("uid");
    const auto cfg_path = tmp / "producer.json";

    // No "uid" field — should be auto-generated with PROD- prefix
    write_file(cfg_path, R"({
        "producer": { "name": "AutoName" },
        "channel":   "lab.auto.channel"
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    EXPECT_FALSE(cfg.producer_uid.empty());
    EXPECT_EQ(cfg.producer_uid.rfind("PROD-", 0), 0u)
        << "Auto-generated uid must start with PROD-, got: " << cfg.producer_uid;

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, FromJsonFile_SchemaFields)
{
    const auto tmp      = unique_temp_dir("schema");
    const auto cfg_path = tmp / "producer.json";

    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-SCHTEST-00000001", "name": "SchTest" },
        "channel": "lab.schema.test",
        "slot_schema": {
            "fields": [
                {"name": "value",     "type": "float32"},
                {"name": "timestamp", "type": "uint64"}
            ]
        },
        "flexzone_schema": {
            "fields": [
                {"name": "meta", "type": "uint8", "count": 64}
            ]
        }
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    ASSERT_FALSE(cfg.slot_schema_json.is_null());
    ASSERT_FALSE(cfg.flexzone_schema_json.is_null());

    const auto &slot_fields = cfg.slot_schema_json["fields"];
    ASSERT_EQ(slot_fields.size(), 2u);
    EXPECT_EQ(slot_fields[0]["name"].get<std::string>(), "value");
    EXPECT_EQ(slot_fields[0]["type"].get<std::string>(), "float32");
    EXPECT_EQ(slot_fields[1]["name"].get<std::string>(), "timestamp");
    EXPECT_EQ(slot_fields[1]["type"].get<std::string>(), "uint64");

    const auto &fz_fields = cfg.flexzone_schema_json["fields"];
    ASSERT_EQ(fz_fields.size(), 1u);
    EXPECT_EQ(fz_fields[0]["name"].get<std::string>(), "meta");
    EXPECT_EQ(fz_fields[0]["count"].get<int>(), 64);

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, FromJsonFile_MissingChannel)
{
    const auto tmp      = unique_temp_dir("nochan");
    const auto cfg_path = tmp / "producer.json";

    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-NOCHAN-00000001", "name": "NoChan" }
    })");

    EXPECT_THROW(
        pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
        std::runtime_error);

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, FromJsonFile_MalformedJson)
{
    const auto tmp      = unique_temp_dir("badjs");
    const auto cfg_path = tmp / "producer.json";

    write_file(cfg_path, R"({ "producer": { broken json )");

    EXPECT_THROW(
        pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
        std::runtime_error);

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, FromJsonFile_FileNotFound)
{
    EXPECT_THROW(
        pylabhub::producer::ProducerConfig::from_json_file(
            "/no/such/path/does/not/exist/producer.json"),
        std::runtime_error);
}

TEST_F(ProducerConfigTest, FromDirectory_Basic)
{
    const auto tmp      = unique_temp_dir("dir");
    const auto cfg_path = tmp / "producer.json";

    // No hub_dir: from_directory() skips hub.json lookup.
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-DIRTEST-00000001", "name": "DirTest" },
        "channel":     "lab.dir.test",
        "interval_ms": 50,
        "script": { "path": "./script", "type": "python" }
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_directory(tmp.string());

    EXPECT_EQ(cfg.producer_uid,  "PROD-DIRTEST-00000001");
    EXPECT_EQ(cfg.channel,       "lab.dir.test");
    EXPECT_EQ(cfg.interval_ms,   50);

    // from_directory() resolves relative script_path to absolute
    EXPECT_TRUE(fs::path(cfg.script_path).is_absolute())
        << "script_path should be absolute after from_directory(), got: " << cfg.script_path;

    fs::remove_all(tmp);
}

// ── Named schema string fields (HEP-CORE-0016 Phase 5) ──────────────────────

TEST_F(ProducerConfigTest, SchemaStringField_StoredAsString)
{
    const auto tmp      = unique_temp_dir("strsch");
    const auto cfg_path = tmp / "producer.json";

    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-STRSCH-00000001", "name": "StrSch" },
        "channel": "lab.strsch.test",
        "slot_schema":     "lab.sensors.temperature.raw@1",
        "flexzone_schema": "lab.sensors.meta@1"
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    ASSERT_TRUE(cfg.slot_schema_json.is_string());
    EXPECT_EQ(cfg.slot_schema_json.get<std::string>(), "lab.sensors.temperature.raw@1");

    ASSERT_TRUE(cfg.flexzone_schema_json.is_string());
    EXPECT_EQ(cfg.flexzone_schema_json.get<std::string>(), "lab.sensors.meta@1");

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, SchemaNullField_StillWorks)
{
    const auto tmp      = unique_temp_dir("nullsch");
    const auto cfg_path = tmp / "producer.json";

    // No schema fields — defaults to null
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-NULLSCH-00000001", "name": "NullSch" },
        "channel": "lab.nullsch.test"
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    EXPECT_TRUE(cfg.slot_schema_json.is_null());
    EXPECT_TRUE(cfg.flexzone_schema_json.is_null());

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, StopOnScriptError_DefaultFalse)
{
    const auto tmp      = unique_temp_dir("def");
    const auto cfg_path = tmp / "producer.json";

    // No "validation" block — all defaults
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-DEFTEST-00000001", "name": "DefTest" },
        "channel": "lab.default.test"
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    EXPECT_FALSE(cfg.stop_on_script_error);
    EXPECT_TRUE(cfg.update_checksum);   // default is true per header

    fs::remove_all(tmp);
}
