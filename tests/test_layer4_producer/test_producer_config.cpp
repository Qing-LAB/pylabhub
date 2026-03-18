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
        "target_period_ms": 200,
        "slot_acquire_timeout_ms":  3000,
        "shm": { "enabled": true, "secret": 12345, "slot_count": 16 },
        "script": { "path": "./script", "type": "python" },
        "validation": { "update_checksum": true, "stop_on_script_error": true }
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    EXPECT_EQ(cfg.producer_uid,    "PROD-TESTSENS-AABBCCDD");
    EXPECT_EQ(cfg.producer_name,   "TestSensor");
    EXPECT_EQ(cfg.log_level,       "debug");
    EXPECT_EQ(cfg.channel,         "lab.test.channel");
    EXPECT_EQ(cfg.target_period_ms, 200);
    EXPECT_EQ(cfg.slot_acquire_timeout_ms,      3000);
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
        "target_period_ms": 50,
        "script": { "path": "./script", "type": "python" }
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_directory(tmp.string());

    EXPECT_EQ(cfg.producer_uid,       "PROD-DIRTEST-00000001");
    EXPECT_EQ(cfg.channel,            "lab.dir.test");
    EXPECT_EQ(cfg.target_period_ms,   50);

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

TEST_F(ProducerConfigTest, Validation_ZeroPeriodIsValidFreeRun)
{
    const auto tmp      = unique_temp_dir("valint");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-VALINT-00000001", "name": "ValInt" },
        "channel": "lab.val.int",
        "target_period_ms": 0
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.target_period_ms, 0); // 0 = free-run, valid
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Validation_NegativePeriodThrows)
{
    const auto tmp      = unique_temp_dir("valn");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-VALN-00000001", "name": "ValN" },
        "channel": "lab.val.neg",
        "target_period_ms": -5
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Validation_BadTimeoutThrows)
{
    const auto tmp      = unique_temp_dir("valtmo");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-VALTMO-00000001", "name": "ValTmo" },
        "channel": "lab.val.tmo",
        "slot_acquire_timeout_ms": -5
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Validation_ZeroSlotCountThrows)
{
    const auto tmp      = unique_temp_dir("valslot");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-VALSLOT-00000001", "name": "ValSlot" },
        "channel": "lab.val.slot",
        "shm": { "enabled": true, "slot_count": 0 }
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── Transport field tests (Phase 1 of producer transport overhaul) ───────────

TEST_F(ProducerConfigTest, Transport_DefaultsToShm)
{
    const auto tmp      = unique_temp_dir("tr_def");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-TRDEF-00000001", "name": "TrDef" },
        "channel": "lab.tr.default"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.transport, pylabhub::producer::Transport::Shm);
    EXPECT_TRUE(cfg.zmq_out_endpoint.empty());
    EXPECT_TRUE(cfg.zmq_out_bind);
    EXPECT_EQ(cfg.zmq_buffer_depth, size_t{64});
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Transport_ParsesZmq)
{
    const auto tmp      = unique_temp_dir("tr_zmq");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-TRZMQ-00000001", "name": "TrZmq" },
        "channel":          "lab.tr.zmq",
        "transport":        "zmq",
        "zmq_out_endpoint": "tcp://0.0.0.0:5581",
        "zmq_out_bind":     true,
        "zmq_buffer_depth": 32,
        "shm": { "enabled": false }
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.transport, pylabhub::producer::Transport::Zmq);
    EXPECT_EQ(cfg.zmq_out_endpoint, "tcp://0.0.0.0:5581");
    EXPECT_TRUE(cfg.zmq_out_bind);
    EXPECT_EQ(cfg.zmq_buffer_depth, size_t{32});
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Transport_MixedShmDefault)
{
    // transport field absent → Shm; ZMQ fields also absent → defaults
    const auto tmp      = unique_temp_dir("tr_mix");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-TRMIX-00000001", "name": "TrMix" },
        "channel": "lab.tr.mix"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.transport, pylabhub::producer::Transport::Shm);
    EXPECT_TRUE(cfg.zmq_out_endpoint.empty());
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Transport_ZmqMissingEndpoint_Throws)
{
    const auto tmp      = unique_temp_dir("tr_noep");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-TRNOEP-00000001", "name": "TrNoEp" },
        "channel":   "lab.tr.noep",
        "transport": "zmq"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Transport_InvalidValue_Throws)
{
    const auto tmp      = unique_temp_dir("tr_bad");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-TRBAD-00000001", "name": "TrBad" },
        "channel":   "lab.tr.bad",
        "transport": "rdma"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Transport_ZmqConnectMode)
{
    // zmq_out_bind=false: PUSH connects instead of binds (e.g. connecting to a PULL aggregator)
    const auto tmp      = unique_temp_dir("tr_conn");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-TRCONN-00000001", "name": "TrConn" },
        "channel":          "lab.tr.conn",
        "transport":        "zmq",
        "zmq_out_endpoint": "tcp://127.0.0.1:5590",
        "zmq_out_bind":     false
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.transport, pylabhub::producer::Transport::Zmq);
    EXPECT_FALSE(cfg.zmq_out_bind);
    EXPECT_EQ(cfg.zmq_out_endpoint, "tcp://127.0.0.1:5590");
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Transport_ZmqZeroBufferDepth_Throws)
{
    const auto tmp      = unique_temp_dir("tr_zerobuf");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-TRZERO-00000001", "name": "TrZero" },
        "channel":          "lab.tr.zero",
        "transport":        "zmq",
        "zmq_out_endpoint": "tcp://127.0.0.1:5591",
        "zmq_buffer_depth": 0
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
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

// ── Inbox field tests (Phase 3 Inbox Facility) ──────────────────────────────

TEST_F(ProducerConfigTest, Inbox_DefaultsToDisabled)
{
    // No "inbox_schema" key — has_inbox() should return false
    const auto tmp      = unique_temp_dir("inbox_off");
    const auto cfg_path = tmp / "producer.json";

    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-INBOXOFF-00000001", "name": "InboxOff" },
        "channel": "lab.inbox.off"
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    EXPECT_FALSE(cfg.has_inbox());
    EXPECT_TRUE(cfg.inbox_schema_json.is_null());
    EXPECT_TRUE(cfg.inbox_endpoint.empty());
    EXPECT_EQ(cfg.inbox_buffer_depth, size_t{64});

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Inbox_ParsesSchema)
{
    // "inbox_schema": {"fields": [{"name":"cmd","type":"uint8"}]}
    const auto tmp      = unique_temp_dir("inbox_sch");
    const auto cfg_path = tmp / "producer.json";

    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-INBOXSCH-00000001", "name": "InboxSch" },
        "channel": "lab.inbox.sch",
        "inbox_schema": {
            "fields": [
                {"name": "cmd", "type": "uint8"}
            ]
        }
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    EXPECT_TRUE(cfg.has_inbox());
    ASSERT_FALSE(cfg.inbox_schema_json.is_null());
    ASSERT_TRUE(cfg.inbox_schema_json.contains("fields"));
    const auto& fields = cfg.inbox_schema_json["fields"];
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields[0]["name"].get<std::string>(), "cmd");
    EXPECT_EQ(fields[0]["type"].get<std::string>(), "uint8");

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Inbox_ParsesEndpointAndDepth)
{
    // "inbox_endpoint": "tcp://0.0.0.0:5592", "inbox_buffer_depth": 32
    const auto tmp      = unique_temp_dir("inbox_ep");
    const auto cfg_path = tmp / "producer.json";

    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-INBOXEP-00000001", "name": "InboxEp" },
        "channel": "lab.inbox.ep",
        "inbox_schema": {"fields": [{"name": "v", "type": "float32"}]},
        "inbox_endpoint": "tcp://0.0.0.0:5592",
        "inbox_buffer_depth": 32
    })");

    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());

    EXPECT_TRUE(cfg.has_inbox());
    EXPECT_EQ(cfg.inbox_endpoint, "tcp://0.0.0.0:5592");
    EXPECT_EQ(cfg.inbox_buffer_depth, size_t{32});

    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Inbox_ZeroBufferDepth_Throws)
{
    // "inbox_buffer_depth": 0 → throws
    const auto tmp      = unique_temp_dir("inbox_zero");
    const auto cfg_path = tmp / "producer.json";

    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-INBOXZERO-00000001", "name": "InboxZero" },
        "channel": "lab.inbox.zero",
        "inbox_schema": {"fields": [{"name": "v", "type": "uint8"}]},
        "inbox_buffer_depth": 0
    })");

    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);

    fs::remove_all(tmp);
}

// ── zmq_packing tests ────────────────────────────────────────────────────────

TEST_F(ProducerConfigTest, ZmqPacking_DefaultsToAligned)
{
    const auto tmp      = unique_temp_dir("zmqpk_def");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-ZMQPKDEF-00000001", "name": "PkDef" },
        "channel": "lab.pk.default"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.zmq_packing, "aligned");
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, ZmqPacking_ParsesPacked)
{
    const auto tmp      = unique_temp_dir("zmqpk_packed");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-ZMQPKPK-00000001", "name": "PkPacked" },
        "channel":          "lab.pk.packed",
        "transport":        "zmq",
        "zmq_out_endpoint": "tcp://127.0.0.1:5592",
        "zmq_packing":      "packed"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.zmq_packing, "packed");
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, ZmqPacking_InvalidValue_Throws)
{
    const auto tmp      = unique_temp_dir("zmqpk_bad");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-ZMQPKBAD-00000001", "name": "PkBad" },
        "channel":     "lab.pk.bad",
        "zmq_packing": "natural"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── zmq_overflow_policy tests ────────────────────────────────────────────────

TEST_F(ProducerConfigTest, ZmqOverflowPolicy_DefaultsDrop)
{
    const auto tmp      = unique_temp_dir("zmqovfl_def");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-ZMQOVDEF-00000001", "name": "OvDef" },
        "channel": "lab.ov.default"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.zmq_overflow_policy, "drop");
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, ZmqOverflowPolicy_ParsesBlock)
{
    const auto tmp      = unique_temp_dir("zmqovfl_block");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":           { "uid": "PROD-ZMQOVBLK-00000001", "name": "OvBlock" },
        "channel":            "lab.ov.block",
        "transport":          "zmq",
        "zmq_out_endpoint":   "tcp://127.0.0.1:5593",
        "zmq_overflow_policy": "block"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.zmq_overflow_policy, "block");
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, ZmqOverflowPolicy_InvalidValue_Throws)
{
    const auto tmp      = unique_temp_dir("zmqovfl_bad");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":            { "uid": "PROD-ZMQOVBAD-00000001", "name": "OvBad" },
        "channel":             "lab.ov.bad",
        "zmq_overflow_policy": "neither"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── FS-02: inbox schema type validation ──────────────────────────────────────

TEST_F(ProducerConfigTest, Inbox_InvalidSchemaType_Integer_Throws)
{
    const auto tmp      = unique_temp_dir("ibschtype_int");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":     { "uid": "PROD-IBSCHINT-00000001", "name": "IbSchInt" },
        "channel":      "lab.ibsch.int",
        "inbox_schema": 42,
        "inbox_endpoint": "tcp://127.0.0.1:9901"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Inbox_InvalidSchemaType_Array_Throws)
{
    const auto tmp      = unique_temp_dir("ibschtype_arr");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":     { "uid": "PROD-IBSCHARR-00000001", "name": "IbSchArr" },
        "channel":      "lab.ibsch.arr",
        "inbox_schema": [{"name": "v", "type": "uint8"}],
        "inbox_endpoint": "tcp://127.0.0.1:9902"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── heartbeat_interval_ms ─────────────────────────────────────────────────────

TEST_F(ProducerConfigTest, HeartbeatIntervalMs_DefaultZero)
{
    const auto tmp      = unique_temp_dir("hbdefprod");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-HBDEF-00000001", "name": "HbDef" },
        "channel":  "lab.hb.test"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.heartbeat_interval_ms, 0);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, HeartbeatIntervalMs_Parsed)
{
    const auto tmp      = unique_temp_dir("hbparsedprod");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":              { "uid": "PROD-HBPARSED-00000001", "name": "HbParsed" },
        "channel":               "lab.hb.test",
        "heartbeat_interval_ms": 1000
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.heartbeat_interval_ms, 1000);
    fs::remove_all(tmp);
}

// ── inbox_overflow_policy ─────────────────────────────────────────────────────

TEST_F(ProducerConfigTest, InboxOverflowPolicy_DefaultDrop)
{
    const auto tmp      = unique_temp_dir("ovfldefprod");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-OVFLDEF-00000001", "name": "OvflDef" },
        "channel":  "lab.ovfl.test"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.inbox_overflow_policy, "drop");
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, InboxOverflowPolicy_ParsesBlock)
{
    const auto tmp      = unique_temp_dir("ovflblockprod");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":              { "uid": "PROD-OVFLBLOCK-00000001", "name": "OvflBlock" },
        "channel":               "lab.ovfl.test",
        "inbox_overflow_policy": "block"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.inbox_overflow_policy, "block");
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, InboxOverflowPolicy_InvalidThrows)
{
    const auto tmp      = unique_temp_dir("ovflinvprod");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":              { "uid": "PROD-OVFLINV-00000001", "name": "OvflInv" },
        "channel":               "lab.ovfl.test",
        "inbox_overflow_policy": "skip"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Inbox_SchemaAsString_Accepted)
{
    const auto tmp      = unique_temp_dir("ibschstr");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":       { "uid": "PROD-IBSCHSTR-00000001", "name": "IbSchStr" },
        "channel":        "lab.ibsch.str",
        "inbox_schema":   "lab/demo/counter.v1",
        "inbox_endpoint": "tcp://127.0.0.1:9903"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_TRUE(cfg.has_inbox());
    EXPECT_TRUE(cfg.inbox_schema_json.is_string());
    fs::remove_all(tmp);
}

// ── LoopTimingPolicy tests ────────────────────────────────────────────────────

TEST_F(ProducerConfigTest, LoopTiming_DefaultFixedRate)
{
    const auto tmp      = unique_temp_dir("lt_def");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-LT-00000001", "name": "LtDef" },
        "channel":  "lab.lt.def"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    // Default target_period_ms=100 → default FixedRate
    EXPECT_EQ(cfg.target_period_ms, 100);
    EXPECT_EQ(cfg.loop_timing, pylabhub::LoopTimingPolicy::FixedRate);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, LoopTiming_MaxRate_Explicit)
{
    const auto tmp      = unique_temp_dir("lt_max");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":         { "uid": "PROD-LT-00000002", "name": "LtMax" },
        "channel":          "lab.lt.max",
        "target_period_ms": 0,
        "loop_timing":      "max_rate"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.target_period_ms, 0);
    EXPECT_EQ(cfg.loop_timing, pylabhub::LoopTimingPolicy::MaxRate);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, LoopTiming_FixedRateWithCompensation)
{
    const auto tmp      = unique_temp_dir("lt_frc");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":         { "uid": "PROD-LT-00000003", "name": "LtFrc" },
        "channel":          "lab.lt.frc",
        "target_period_ms": 50,
        "loop_timing":      "fixed_rate_with_compensation"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.target_period_ms, 50);
    EXPECT_EQ(cfg.loop_timing, pylabhub::LoopTimingPolicy::FixedRateWithCompensation);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, LoopTiming_MaxRate_WithPeriod_Throws)
{
    const auto tmp      = unique_temp_dir("lt_max_inv");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":         { "uid": "PROD-LT-00000004", "name": "LtMaxInv" },
        "channel":          "lab.lt.maxinv",
        "target_period_ms": 100,
        "loop_timing":      "max_rate"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, LoopTiming_FixedRate_ZeroPeriod_Throws)
{
    const auto tmp      = unique_temp_dir("lt_fr_inv");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":         { "uid": "PROD-LT-00000005", "name": "LtFrInv" },
        "channel":          "lab.lt.frinv",
        "target_period_ms": 0,
        "loop_timing":      "fixed_rate"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, LoopTiming_InvalidValue_Throws)
{
    const auto tmp      = unique_temp_dir("lt_inv");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":    { "uid": "PROD-LT-00000006", "name": "LtInv" },
        "channel":     "lab.lt.inv",
        "loop_timing": "periodic"
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── Monitoring config fields ──────────────────────────────────────────────────

TEST_F(ProducerConfigTest, MonitoringFields_Defaults)
{
    const auto tmp      = unique_temp_dir("mon_def");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-MON-00000001", "name": "MonDef" },
        "channel":  "lab.mon.def"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.ctrl_queue_max_depth, size_t{256});
    EXPECT_EQ(cfg.peer_dead_timeout_ms, 30000);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, MonitoringFields_Explicit)
{
    const auto tmp      = unique_temp_dir("mon_exp");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":            { "uid": "PROD-MON-00000002", "name": "MonExp" },
        "channel":             "lab.mon.exp",
        "ctrl_queue_max_depth": 128,
        "peer_dead_timeout_ms": 10000
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.ctrl_queue_max_depth, size_t{128});
    EXPECT_EQ(cfg.peer_dead_timeout_ms, 10000);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, MonitoringFields_Disabled)
{
    const auto tmp      = unique_temp_dir("mon_dis");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer":            { "uid": "PROD-MON-00000003", "name": "MonDis" },
        "channel":             "lab.mon.dis",
        "peer_dead_timeout_ms": 0
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.peer_dead_timeout_ms, 0);
    fs::remove_all(tmp);
}

// ── startup.wait_for_roles ─────────────────────────────────────────────────────

TEST_F(ProducerConfigTest, Startup_DefaultsToEmpty)
{
    const auto tmp      = unique_temp_dir("su_def");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-SUDEF-00000001", "name": "SuDef" },
        "channel":  "lab.su.def"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_TRUE(cfg.wait_for_roles.empty());
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Startup_ParsesSingleRole)
{
    const auto tmp      = unique_temp_dir("su_one");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-SUONE-00000001", "name": "SuOne" },
        "channel":  "lab.su.one",
        "startup": {
            "wait_for_roles": [
                { "uid": "CONS-LOGGER-AABBCCDD", "timeout_ms": 5000 }
            ]
        }
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    ASSERT_EQ(cfg.wait_for_roles.size(), size_t{1});
    EXPECT_EQ(cfg.wait_for_roles[0].uid,        "CONS-LOGGER-AABBCCDD");
    EXPECT_EQ(cfg.wait_for_roles[0].timeout_ms, 5000);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Startup_ParsesMultipleRoles)
{
    const auto tmp      = unique_temp_dir("su_multi");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-SUMULTI-00000001", "name": "SuMulti" },
        "channel":  "lab.su.multi",
        "startup": {
            "wait_for_roles": [
                { "uid": "CONS-LOGGER-AABBCCDD", "timeout_ms": 5000 },
                { "uid": "PROC-SCALER-11223344", "timeout_ms": 3000 }
            ]
        }
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    ASSERT_EQ(cfg.wait_for_roles.size(), size_t{2});
    EXPECT_EQ(cfg.wait_for_roles[0].uid,        "CONS-LOGGER-AABBCCDD");
    EXPECT_EQ(cfg.wait_for_roles[0].timeout_ms, 5000);
    EXPECT_EQ(cfg.wait_for_roles[1].uid,        "PROC-SCALER-11223344");
    EXPECT_EQ(cfg.wait_for_roles[1].timeout_ms, 3000);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Startup_DefaultTimeout)
{
    const auto tmp      = unique_temp_dir("su_deftmo");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-SUDEFTMO-00000001", "name": "SuDefTmo" },
        "channel":  "lab.su.deftmo",
        "startup": {
            "wait_for_roles": [
                { "uid": "CONS-LOGGER-AABBCCDD" }
            ]
        }
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    ASSERT_EQ(cfg.wait_for_roles.size(), size_t{1});
    EXPECT_EQ(cfg.wait_for_roles[0].timeout_ms, pylabhub::kDefaultStartupWaitTimeoutMs);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, Startup_EmptyUid_Throws)
{
    const auto tmp      = unique_temp_dir("su_emptyuid");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-SUEUID-00000001", "name": "SuEuid" },
        "channel":  "lab.su.euid",
        "startup": {
            "wait_for_roles": [ { "uid": "", "timeout_ms": 5000 } ]
        }
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}


TEST_F(ProducerConfigTest, Startup_MaxTimeout_Throws)
{
    const auto tmp      = unique_temp_dir("su_maxtmo");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-SUMXTMO-00000001", "name": "SuMxtmo" },
        "channel":  "lab.su.mxtmo",
        "startup": {
            "wait_for_roles": [ { "uid": "CONS-LOGGER-AABBCCDD", "timeout_ms": 3600001 } ]
        }
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}
TEST_F(ProducerConfigTest, Startup_ZeroTimeout_Throws)
{
    const auto tmp      = unique_temp_dir("su_zerotmo");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-SUZTMO-00000001", "name": "SuZtmo" },
        "channel":  "lab.su.ztmo",
        "startup": {
            "wait_for_roles": [ { "uid": "CONS-LOGGER-AABBCCDD", "timeout_ms": 0 } ]
        }
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

// ── reader_sync_policy tests ─────────────────────────────────────────────────

TEST_F(ProducerConfigTest, ReaderSyncPolicy_DefaultSequential)
{
    const auto tmp      = unique_temp_dir("rsp_def");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-RSPDEF-00000001", "name": "RspDef" },
        "channel":  "lab.rsp.def",
        "shm": { "enabled": true, "slot_count": 8 }
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.shm_consumer_sync_policy, pylabhub::hub::ConsumerSyncPolicy::Sequential);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, ReaderSyncPolicy_ExplicitSequential)
{
    const auto tmp      = unique_temp_dir("rsp_seq");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-RSPSEQ-00000001", "name": "RspSeq" },
        "channel":  "lab.rsp.seq",
        "shm": { "enabled": true, "slot_count": 8, "reader_sync_policy": "sequential" }
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.shm_consumer_sync_policy, pylabhub::hub::ConsumerSyncPolicy::Sequential);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, ReaderSyncPolicy_LatestOnly)
{
    const auto tmp      = unique_temp_dir("rsp_lo");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-RSPLO-00000001", "name": "RspLo" },
        "channel":  "lab.rsp.lo",
        "shm": { "enabled": true, "slot_count": 8, "reader_sync_policy": "latest_only" }
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.shm_consumer_sync_policy, pylabhub::hub::ConsumerSyncPolicy::Latest_only);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, ReaderSyncPolicy_SequentialSync)
{
    const auto tmp      = unique_temp_dir("rsp_seqsync");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-RSPSS-00000001", "name": "RspSS" },
        "channel":  "lab.rsp.seqsync",
        "shm": { "enabled": true, "slot_count": 8, "reader_sync_policy": "sequential_sync" }
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.shm_consumer_sync_policy, pylabhub::hub::ConsumerSyncPolicy::Sequential_sync);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, ReaderSyncPolicy_InvalidValue_Throws)
{
    const auto tmp      = unique_temp_dir("rsp_inv");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-RSPINV-00000001", "name": "RspInv" },
        "channel":  "lab.rsp.inv",
        "shm": { "enabled": true, "slot_count": 8, "reader_sync_policy": "round_robin" }
    })");
    EXPECT_THROW(pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string()),
                 std::runtime_error);
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, ReaderSyncPolicy_NoShmBlock_DefaultSequential)
{
    const auto tmp      = unique_temp_dir("rsp_noshm");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-RSPNOSHM-00000001", "name": "RspNoShm" },
        "channel":  "lab.rsp.noshm"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.shm_consumer_sync_policy, pylabhub::hub::ConsumerSyncPolicy::Sequential);
    fs::remove_all(tmp);
}

// ── Python venv field tests ──────────────────────────────────────────────────

TEST_F(ProducerConfigTest, PythonVenv_DefaultEmpty)
{
    const auto tmp      = unique_temp_dir("venv_def");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-VENVDEF-00000001", "name": "VenvDef" },
        "channel": "lab.venv.def"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_TRUE(cfg.python_venv.empty());
    fs::remove_all(tmp);
}

TEST_F(ProducerConfigTest, PythonVenv_ParsesName)
{
    const auto tmp      = unique_temp_dir("venv_name");
    const auto cfg_path = tmp / "producer.json";
    write_file(cfg_path, R"({
        "producer": { "uid": "PROD-VENVNAME-00000001", "name": "VenvName" },
        "channel": "lab.venv.name",
        "python_venv": "my-data-env"
    })");
    const auto cfg = pylabhub::producer::ProducerConfig::from_json_file(cfg_path.string());
    EXPECT_EQ(cfg.python_venv, "my-data-env");
    fs::remove_all(tmp);
}
