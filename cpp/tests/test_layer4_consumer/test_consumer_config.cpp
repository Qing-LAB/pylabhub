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

TEST_F(ConsumerConfigTest, Validation_BadTimeoutThrows)
{
    const auto tmp      = unique_temp_dir("valtmo");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-VALTMO-00000001", "name": "ValTmo" },
        "channel":    "lab.val.tmo",
        "timeout_ms": -5
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── Loop driver / timing tests ────────────────────────────────────────────────

TEST_F(ConsumerConfigTest, QueueType_DefaultsToShm)
{
    const auto tmp      = unique_temp_dir("ld_default");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-LD-00000001", "name": "LdDefault" },
        "channel":   "lab.ld.default"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.queue_type,     pylabhub::consumer::QueueType::Shm);
    EXPECT_EQ(cfg.target_period_ms, 0);
    EXPECT_EQ(cfg.loop_timing,     pylabhub::LoopTimingPolicy::MaxRate);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, QueueType_ParsesZmq)
{
    const auto tmp      = unique_temp_dir("ld_zmq");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":   { "uid": "CONS-LD-00000002", "name": "LdZmq" },
        "channel":    "lab.ld.zmq",
        "queue_type": "zmq"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.queue_type, pylabhub::consumer::QueueType::Zmq);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, QueueType_InvalidThrows)
{
    const auto tmp      = unique_temp_dir("ld_inv");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":    { "uid": "CONS-LD-00000003", "name": "LdInv" },
        "channel":     "lab.ld.inv",
        "queue_type": "timer"
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, TargetPeriodMs_Parsed)
{
    const auto tmp      = unique_temp_dir("tp_ms");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":         { "uid": "CONS-TP-00000001", "name": "TpMs" },
        "channel":          "lab.tp.ms",
        "target_period_ms": 50,
        "loop_timing":      "fixed_rate_with_compensation"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.target_period_ms, 50);
    EXPECT_EQ(cfg.loop_timing, pylabhub::LoopTimingPolicy::FixedRateWithCompensation);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, TargetPeriodMs_ZeroIsValidFreeRun)
{
    const auto tmp      = unique_temp_dir("tp_zero");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":         { "uid": "CONS-TP-00000002", "name": "TpZero" },
        "channel":          "lab.tp.zero",
        "target_period_ms": 0
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.target_period_ms, 0);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, TargetPeriodMs_NegativeThrows)
{
    const auto tmp      = unique_temp_dir("tp_neg");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":         { "uid": "CONS-TP-00000003", "name": "TpNeg" },
        "channel":          "lab.tp.neg",
        "target_period_ms": -1
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, LoopTiming_InvalidThrows)
{
    const auto tmp      = unique_temp_dir("lt_inv");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":    { "uid": "CONS-LT-00000001", "name": "LtInv" },
        "channel":     "lab.lt.inv",
        "loop_timing": "periodic"
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, LoopTiming_MaxRate_Explicit)
{
    const auto tmp      = unique_temp_dir("lt_max");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":         { "uid": "CONS-LT-00000002", "name": "LtMax" },
        "channel":          "lab.lt.max",
        "target_period_ms": 0,
        "loop_timing":      "max_rate"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.target_period_ms, 0);
    EXPECT_EQ(cfg.loop_timing, pylabhub::LoopTimingPolicy::MaxRate);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, LoopTiming_FixedRate_Explicit)
{
    const auto tmp      = unique_temp_dir("lt_fr");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":         { "uid": "CONS-LT-00000003", "name": "LtFr" },
        "channel":          "lab.lt.fr",
        "target_period_ms": 100,
        "loop_timing":      "fixed_rate"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.target_period_ms, 100);
    EXPECT_EQ(cfg.loop_timing, pylabhub::LoopTimingPolicy::FixedRate);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, LoopTiming_MaxRate_WithPeriod_Throws)
{
    const auto tmp      = unique_temp_dir("lt_max_inv");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":         { "uid": "CONS-LT-00000004", "name": "LtMaxInv" },
        "channel":          "lab.lt.maxinv",
        "target_period_ms": 50,
        "loop_timing":      "max_rate"
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, LoopTiming_FixedRate_ZeroPeriod_Throws)
{
    const auto tmp      = unique_temp_dir("lt_fr_inv");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":         { "uid": "CONS-LT-00000005", "name": "LtFrInv" },
        "channel":          "lab.lt.frinv",
        "target_period_ms": 0,
        "loop_timing":      "fixed_rate"
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── Inbox facility tests ──────────────────────────────────────────────────────

TEST_F(ConsumerConfigTest, ConsumerConfig_NoInbox_DefaultFields)
{
    const auto tmp      = unique_temp_dir("noinbox");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-NOINBOX-00000001", "name": "NoInbox" },
        "channel":  "lab.noinbox.test"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_FALSE(cfg.has_inbox());
    EXPECT_TRUE(cfg.inbox_schema_json.is_null());
    EXPECT_EQ(cfg.inbox_endpoint,     "");
    EXPECT_EQ(cfg.inbox_buffer_depth, size_t{64});
    EXPECT_EQ(cfg.zmq_packing,        "aligned");
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, ConsumerConfig_InboxSchema_ParsedCorrectly)
{
    const auto tmp      = unique_temp_dir("ibschema");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":     { "uid": "CONS-IBSCH-00000001", "name": "IbSch" },
        "channel":      "lab.ibsch.test",
        "inbox_schema": { "fields": [{"name": "value", "type": "float32"}] }
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_TRUE(cfg.has_inbox());
    ASSERT_FALSE(cfg.inbox_schema_json.is_null());
    const auto &fields = cfg.inbox_schema_json["fields"];
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields[0]["name"].get<std::string>(), "value");
    EXPECT_EQ(fields[0]["type"].get<std::string>(), "float32");
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, ConsumerConfig_InboxEndpoint_Custom)
{
    const auto tmp      = unique_temp_dir("ibep");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":       { "uid": "CONS-IBEP-00000001", "name": "IbEp" },
        "channel":        "lab.ibep.test",
        "inbox_schema":   { "fields": [{"name": "x", "type": "int32"}] },
        "inbox_endpoint": "tcp://127.0.0.1:9900"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_TRUE(cfg.has_inbox());
    EXPECT_EQ(cfg.inbox_endpoint, "tcp://127.0.0.1:9900");
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, ConsumerConfig_ZmqPacking_Parsed)
{
    const auto tmp      = unique_temp_dir("zmqpack");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":    { "uid": "CONS-ZMQPK-00000001", "name": "ZmqPk" },
        "channel":     "lab.zmqpk.test",
        "zmq_packing": "packed"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.zmq_packing, "packed");
    fs::remove_all(tmp);
}

// ── FS-02: ConsumerConfig validation ─────────────────────────────────────────

TEST_F(ConsumerConfigTest, ConsumerConfig_InvalidZmqPacking_Throws)
{
    const auto tmp      = unique_temp_dir("badzmqpk");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":    { "uid": "CONS-BADPK-00000001", "name": "BadPk" },
        "channel":     "lab.badpk.test",
        "zmq_packing": "natural"
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, ConsumerConfig_ZeroInboxBufferDepth_Throws)
{
    const auto tmp      = unique_temp_dir("ibdepth0");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":          { "uid": "CONS-IBDEPTH0-00000001", "name": "IbDepth0" },
        "channel":           "lab.ibdepth0.test",
        "inbox_schema":      { "fields": [{"name": "v", "type": "uint8"}] },
        "inbox_buffer_depth": 0
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, ConsumerConfig_InvalidInboxSchemaType_Integer_Throws)
{
    const auto tmp      = unique_temp_dir("ibschint");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":       { "uid": "CONS-IBSCHINT-00000001", "name": "IbSchInt" },
        "channel":        "lab.ibschint.test",
        "inbox_schema":   42,
        "inbox_endpoint": "tcp://127.0.0.1:9904"
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, ConsumerConfig_InvalidInboxSchemaType_Array_Throws)
{
    const auto tmp      = unique_temp_dir("ibscharr");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":       { "uid": "CONS-IBSCHARR-00000001", "name": "IbSchArr" },
        "channel":        "lab.ibscharr.test",
        "inbox_schema":   [{"name": "v", "type": "uint8"}],
        "inbox_endpoint": "tcp://127.0.0.1:9905"
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, ConsumerConfig_InboxSchemaAsString_Accepted)
{
    const auto tmp      = unique_temp_dir("ibschstrcons");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":       { "uid": "CONS-IBSCHSTR-00000001", "name": "IbSchStr" },
        "channel":        "lab.ibschstr.test",
        "inbox_schema":   "lab/demo/counter.v1",
        "inbox_endpoint": "tcp://127.0.0.1:9906"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_TRUE(cfg.has_inbox());
    EXPECT_TRUE(cfg.inbox_schema_json.is_string());
    fs::remove_all(tmp);
}

// ── verify_checksum ───────────────────────────────────────────────────────────

TEST_F(ConsumerConfigTest, VerifyChecksum_DefaultFalse)
{
    const auto tmp      = unique_temp_dir("vchkdef");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-VCHKDEF-00000001", "name": "VChkDef" },
        "channel":  "lab.vchk.test"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_FALSE(cfg.verify_checksum);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, VerifyChecksum_ParsedTrue)
{
    const auto tmp      = unique_temp_dir("vchkon");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":    { "uid": "CONS-VCHKON-00000001", "name": "VChkOn" },
        "channel":     "lab.vchk.test",
        "validation":  { "verify_checksum": true }
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_TRUE(cfg.verify_checksum);
    fs::remove_all(tmp);
}

// ── heartbeat_interval_ms ─────────────────────────────────────────────────────

TEST_F(ConsumerConfigTest, HeartbeatIntervalMs_DefaultZero)
{
    const auto tmp      = unique_temp_dir("hbdef");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-HBDEF-00000001", "name": "HbDef" },
        "channel":  "lab.hb.test"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.heartbeat_interval_ms, 0);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, HeartbeatIntervalMs_Parsed)
{
    const auto tmp      = unique_temp_dir("hbparsed");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":              { "uid": "CONS-HBPARSED-00000001", "name": "HbParsed" },
        "channel":               "lab.hb.test",
        "heartbeat_interval_ms": 2500
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.heartbeat_interval_ms, 2500);
    fs::remove_all(tmp);
}

// ── zmq_buffer_depth ─────────────────────────────────────────────────────────

TEST_F(ConsumerConfigTest, ZmqBufferDepth_DefaultIs64)
{
    const auto tmp      = unique_temp_dir("zmqbddef");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-ZMQBDDEF-00000001", "name": "ZmqBdDef" },
        "channel":  "lab.zmqbd.test"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.zmq_buffer_depth, size_t{64});
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, ZmqBufferDepth_Parsed)
{
    const auto tmp      = unique_temp_dir("zmqbdparsed");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":        { "uid": "CONS-ZMQBDPARSED-00000001", "name": "ZmqBdParsed" },
        "channel":         "lab.zmqbd.test",
        "zmq_buffer_depth": 128
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.zmq_buffer_depth, size_t{128});
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, ZmqBufferDepth_ZeroThrows)
{
    const auto tmp      = unique_temp_dir("zmqbdzero");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":        { "uid": "CONS-ZMQBDZERO-00000001", "name": "ZmqBdZero" },
        "channel":         "lab.zmqbd.test",
        "zmq_buffer_depth": 0
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── inbox_overflow_policy ─────────────────────────────────────────────────────

TEST_F(ConsumerConfigTest, InboxOverflowPolicy_DefaultDrop)
{
    const auto tmp      = unique_temp_dir("ovfldef");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-OVFLDEF-00000001", "name": "OvflDef" },
        "channel":  "lab.ovfl.test"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.inbox_overflow_policy, "drop");
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, InboxOverflowPolicy_ParsesBlock)
{
    const auto tmp      = unique_temp_dir("ovflblock");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":              { "uid": "CONS-OVFLBLOCK-00000001", "name": "OvflBlock" },
        "channel":               "lab.ovfl.test",
        "inbox_overflow_policy": "block"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.inbox_overflow_policy, "block");
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, InboxOverflowPolicy_InvalidThrows)
{
    const auto tmp      = unique_temp_dir("ovflinvalid");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":              { "uid": "CONS-OVFLINV-00000001", "name": "OvflInv" },
        "channel":               "lab.ovfl.test",
        "inbox_overflow_policy": "skip"
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── Monitoring config fields ──────────────────────────────────────────────────

TEST_F(ConsumerConfigTest, MonitoringFields_Defaults)
{
    const auto tmp      = unique_temp_dir("mon_def");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-MON-00000001", "name": "MonDef" },
        "channel":  "lab.mon.def"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.ctrl_queue_max_depth, size_t{256});
    EXPECT_EQ(cfg.peer_dead_timeout_ms, 30000);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, MonitoringFields_Explicit)
{
    const auto tmp      = unique_temp_dir("mon_exp");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":            { "uid": "CONS-MON-00000002", "name": "MonExp" },
        "channel":             "lab.mon.exp",
        "ctrl_queue_max_depth": 64,
        "peer_dead_timeout_ms": 5000
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.ctrl_queue_max_depth, size_t{64});
    EXPECT_EQ(cfg.peer_dead_timeout_ms, 5000);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, MonitoringFields_Disabled)
{
    const auto tmp      = unique_temp_dir("mon_dis");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer":            { "uid": "CONS-MON-00000003", "name": "MonDis" },
        "channel":             "lab.mon.dis",
        "peer_dead_timeout_ms": 0
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.peer_dead_timeout_ms, 0);
    fs::remove_all(tmp);
}

// ── startup.wait_for_roles ─────────────────────────────────────────────────────

TEST_F(ConsumerConfigTest, Startup_DefaultsToEmpty)
{
    const auto tmp      = unique_temp_dir("su_def");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-SUDEF-00000001", "name": "SuDef" },
        "channel":  "lab.su.def"
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    EXPECT_TRUE(cfg.wait_for_roles.empty());
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, Startup_ParsesSingleRole)
{
    const auto tmp      = unique_temp_dir("su_one");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-SUONE-00000001", "name": "SuOne" },
        "channel":  "lab.su.one",
        "startup": {
            "wait_for_roles": [
                { "uid": "PROD-SENSOR-AABBCCDD", "timeout_ms": 8000 }
            ]
        }
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    ASSERT_EQ(cfg.wait_for_roles.size(), size_t{1});
    EXPECT_EQ(cfg.wait_for_roles[0].uid,        "PROD-SENSOR-AABBCCDD");
    EXPECT_EQ(cfg.wait_for_roles[0].timeout_ms, 8000);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, Startup_DefaultTimeout)
{
    const auto tmp      = unique_temp_dir("su_deftmo");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-SUDEFTMO-00000001", "name": "SuDefTmo" },
        "channel":  "lab.su.deftmo",
        "startup": {
            "wait_for_roles": [ { "uid": "PROD-SENSOR-AABBCCDD" } ]
        }
    })");
    const auto cfg = pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string());
    ASSERT_EQ(cfg.wait_for_roles.size(), size_t{1});
    EXPECT_EQ(cfg.wait_for_roles[0].timeout_ms, pylabhub::kDefaultStartupWaitTimeoutMs);
    fs::remove_all(tmp);
}

TEST_F(ConsumerConfigTest, Startup_EmptyUid_Throws)
{
    const auto tmp      = unique_temp_dir("su_emptyuid");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-SUEUID-00000001", "name": "SuEuid" },
        "channel":  "lab.su.euid",
        "startup": {
            "wait_for_roles": [ { "uid": "", "timeout_ms": 5000 } ]
        }
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}


TEST_F(ConsumerConfigTest, Startup_MaxTimeout_Throws)
{
    const auto tmp      = unique_temp_dir("su_maxtmo");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-SUMXTMO-00000001", "name": "SuMxtmo" },
        "channel":  "lab.su.mxtmo",
        "startup": {
            "wait_for_roles": [ { "uid": "CONS-LOGGER-AABBCCDD", "timeout_ms": 3600001 } ]
        }
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}
TEST_F(ConsumerConfigTest, Startup_ZeroTimeout_Throws)
{
    const auto tmp      = unique_temp_dir("su_zerotmo");
    const auto cfg_path = tmp / "consumer.json";
    write_file(cfg_path, R"({
        "consumer": { "uid": "CONS-SUZTMO-00000001", "name": "SuZtmo" },
        "channel":  "lab.su.ztmo",
        "startup": {
            "wait_for_roles": [ { "uid": "PROD-SENSOR-AABBCCDD", "timeout_ms": 0 } ]
        }
    })");
    EXPECT_THROW(pylabhub::consumer::ConsumerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}
