/**
 * @file test_consumer_config.cpp
 * @brief Unit tests for ConsumerConfig JSON parsing (no lifecycle, no Python).
 *
 * consumer_config.cpp is compiled directly into this test binary.
 *
 * Pattern 1 (PureApiTest) — no module init, no lifecycle.
 */

#include "consumer_config.hpp"

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
                   / ("plh_conscfg_" + prefix + "_" + std::to_string(id));
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path &path, const std::string &content)
{
    std::ofstream f(path);
    f << content;
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class ConsumerConfigTest : public pylabhub::tests::PureApiTest {};

// ── Tests ────────────────────────────────────────────────────────────────────

TEST_F(ConsumerConfigTest, FromJsonFile_Basic)
{
    const auto tmp      = unique_temp_dir("basic");
    const auto cfg_path = tmp / "consumer.json";

    write_file(cfg_path, R"({
        "consumer": {
            "uid":       "CONS-LOGGER-AABBCCDD",
            "name":      "Logger",
            "log_level": "debug"
        },
        "channel":    "lab.test.channel",
        "timeout_ms": 3000,
        "shm": { "enabled": true, "secret": 99999 },
        "script": { "path": "./script", "type": "python" },
        "validation": { "stop_on_script_error": true }
    })");

    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());

    EXPECT_EQ(cfg.consumer_uid,  "CONS-LOGGER-AABBCCDD");
    EXPECT_EQ(cfg.consumer_name, "Logger");
    EXPECT_EQ(cfg.log_level,     "debug");
    EXPECT_EQ(cfg.channel,       "lab.test.channel");
    EXPECT_EQ(cfg.timeout_ms,    3000);
    EXPECT_TRUE(cfg.shm_enabled);
    EXPECT_EQ(cfg.shm_secret,    uint64_t{99999});
    EXPECT_EQ(cfg.script_path,   "./script");
    EXPECT_EQ(cfg.script_type,   "python");
    EXPECT_TRUE(cfg.stop_on_script_error);

    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, FromJsonFile_UidAutoGen)
{
    const auto tmp      = unique_temp_dir("uid");
    const auto cfg_path = tmp / "consumer.json";

    // No "uid" field — should be auto-generated with CONS- prefix
    write_file(cfg_path, R"({
        "consumer": { "name": "AutoLogger" },
        "channel":   "lab.auto.channel"
    })");

    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());

    EXPECT_FALSE(cfg.consumer_uid.empty());
    EXPECT_EQ(cfg.consumer_uid.rfind("CONS-", 0), 0u)
        << "Auto-generated uid must start with CONS-, got: " << cfg.consumer_uid;

    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, FromJsonFile_SchemaFields)
{
    const auto tmp      = unique_temp_dir("schema");
    const auto cfg_path = tmp / "consumer.json";

    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-SCHTEST-00000001", "name": "SchTest" },
        "channel": "lab.schema.test",
        "slot_schema": {
            "fields": [
                {"name": "value",     "type": "float32"},
                {"name": "timestamp", "type": "uint64"}
            ]
        },
        "flexzone_schema": {
            "fields": [
                {"name": "header", "type": "uint32", "count": 4}
            ]
        }
    })");

    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());

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
    EXPECT_EQ(fz_fields[0]["name"].get<std::string>(), "header");
    EXPECT_EQ(fz_fields[0]["count"].get<int>(), 4);

    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, FromJsonFile_MissingChannel)
{
    const auto tmp      = unique_temp_dir("nochan");
    const auto cfg_path = tmp / "consumer.json";

    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-NOCHAN-00000001", "name": "NoChan" }
    })");

    EXPECT_THROW(
        pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
        std::runtime_error);

    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, FromJsonFile_MalformedJson)
{
    const auto tmp      = unique_temp_dir("badjs");
    const auto cfg_path = tmp / "consumer.json";

    write_file(cfg_path, R"({ "consumer": { broken json )");

    EXPECT_THROW(
        pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
        std::runtime_error);

    fs::remove_all(tmp);
}

// ── Named schema string fields (HEP-CORE-0016 Phase 5) ──────────────────────

TEST_F(ConsumerConfigTest, SchemaStringField_StoredAsString)
{
    const auto tmp      = unique_temp_dir("strsch");
    const auto cfg_path = tmp / "consumer.json";

    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-STRSCH-00000001", "name": "StrSch" },
        "channel": "lab.strsch.test",
        "slot_schema":     "lab.sensors.temperature.raw@1",
        "flexzone_schema": "lab.sensors.meta@1"
    })");

    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());

    ASSERT_TRUE(cfg.slot_schema_json.is_string());
    EXPECT_EQ(cfg.slot_schema_json.get<std::string>(), "lab.sensors.temperature.raw@1");

    ASSERT_TRUE(cfg.flexzone_schema_json.is_string());
    EXPECT_EQ(cfg.flexzone_schema_json.get<std::string>(), "lab.sensors.meta@1");

    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, SchemaNullField_StillWorks)
{
    const auto tmp      = unique_temp_dir("nullsch");
    const auto cfg_path = tmp / "consumer.json";

    // No schema fields — defaults to null
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-NULLSCH-00000001", "name": "NullSch" },
        "channel": "lab.nullsch.test"
    })");

    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());

    EXPECT_TRUE(cfg.slot_schema_json.is_null());
    EXPECT_TRUE(cfg.flexzone_schema_json.is_null());

    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, FromDirectory_Basic)
{
    const auto tmp      = unique_temp_dir("dir");
    const auto cfg_path = tmp / "consumer.json";

    // No hub_dir: from_directory() skips hub.json lookup.
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-DIRTEST-00000001", "name": "DirTest" },
        "channel":    "lab.dir.test",
        "timeout_ms": 1000,
        "script": { "path": "./script", "type": "python" }
    })");

    const auto cfg = pylabhub::consumer::ConsumerConfig::from_directory(tmp.string());

    EXPECT_EQ(cfg.consumer_uid,  "CONS-DIRTEST-00000001");
    EXPECT_EQ(cfg.channel,       "lab.dir.test");
    EXPECT_EQ(cfg.timeout_ms,    1000);

    // from_directory() resolves relative script_path to absolute
    EXPECT_TRUE(fs::path(cfg.script_path).is_absolute())
        << "script_path should be absolute after from_directory(), got: " << cfg.script_path;

    fs::remove_all(tmp);
}
