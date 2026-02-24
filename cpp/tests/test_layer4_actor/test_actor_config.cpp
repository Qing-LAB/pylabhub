/**
 * @file test_actor_config.cpp
 * @brief Layer 4 tests for ActorConfig::from_json_file() parsing.
 *
 * Tests cover:
 *   - loop_timing field: "fixed_pace", "compensating", absent (default), invalid
 *   - broker_pubkey field: present / absent
 *   - broker endpoint default and custom
 *   - interval_ms / timeout_ms values
 *   - validation block: all four policy fields, defaults, invalid values
 *   - actor block: uid present, uid absent (auto-gen), non-ACTOR- prefix (warning, no throw)
 *   - Multi-role configs
 *   - Error cases: missing required fields, invalid values, bad file path, malformed JSON
 *
 * No lifecycle init needed: LOGGER_COMPILE_LEVEL=0 makes all LOGGER_* calls no-ops.
 */

#include "actor_config.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace pylabhub::actor;

// ============================================================================
// Helpers
// ============================================================================

namespace
{

/// RAII temporary JSON file — writes content to a unique temp path; removes on destruction.
class TmpJsonFile
{
public:
    explicit TmpJsonFile(const std::string &content)
    {
        static std::atomic<int> counter{0};
        path_ = (std::filesystem::temp_directory_path() /
                 ("actor_cfg_test_" + std::to_string(++counter) + ".json")).string();
        std::ofstream f(path_);
        f << content;
    }

    ~TmpJsonFile()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    const std::string &path() const { return path_; }

private:
    std::string path_;
};

/// Build a minimal valid multi-role producer config.
/// @p extra_role_fields is appended (with a leading comma) inside the "out" role object.
std::string make_producer_json(const std::string &extra_role_fields = "")
{
    std::string role_body = R"("kind": "producer", "channel": "lab.test")";
    if (!extra_role_fields.empty())
        role_body += ",\n      " + extra_role_fields;

    return
        "{\n"
        "  \"script\": \"test.py\",\n"
        "  \"actor\": {\"uid\": \"ACTOR-TEST-12345678\", \"name\": \"TestActor\"},\n"
        "  \"roles\": {\n"
        "    \"out\": {\n"
        "      " + role_body + "\n"
        "    }\n"
        "  }\n"
        "}";
}

/// Build a minimal valid multi-role consumer config.
std::string make_consumer_json(const std::string &extra_role_fields = "")
{
    std::string role_body = R"("kind": "consumer", "channel": "lab.test")";
    if (!extra_role_fields.empty())
        role_body += ",\n      " + extra_role_fields;

    return
        "{\n"
        "  \"script\": \"test.py\",\n"
        "  \"actor\": {\"uid\": \"ACTOR-TEST-12345678\", \"name\": \"TestActor\"},\n"
        "  \"roles\": {\n"
        "    \"in\": {\n"
        "      " + role_body + "\n"
        "    }\n"
        "  }\n"
        "}";
}

} // anonymous namespace

// ============================================================================
// LoopTimingPolicy parsing
// ============================================================================

TEST(ActorConfigLoopTiming, DefaultIsFixedPace)
{
    TmpJsonFile f(make_producer_json());
    const auto cfg = ActorConfig::from_json_file(f.path());
    ASSERT_EQ(cfg.roles.count("out"), 1u);
    EXPECT_EQ(cfg.roles.at("out").loop_timing, RoleConfig::LoopTimingPolicy::FixedPace);
}

TEST(ActorConfigLoopTiming, ExplicitFixedPace)
{
    TmpJsonFile f(make_producer_json(R"("loop_timing": "fixed_pace")"));
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("out").loop_timing, RoleConfig::LoopTimingPolicy::FixedPace);
}

TEST(ActorConfigLoopTiming, Compensating)
{
    TmpJsonFile f(make_producer_json(R"("loop_timing": "compensating")"));
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("out").loop_timing, RoleConfig::LoopTimingPolicy::Compensating);
}

TEST(ActorConfigLoopTiming, InvalidValueThrows)
{
    TmpJsonFile f(make_producer_json(R"("loop_timing": "turbo")"));
    EXPECT_THROW(ActorConfig::from_json_file(f.path()), std::runtime_error);
}

TEST(ActorConfigLoopTiming, ConsumerDefaultIsFixedPace)
{
    TmpJsonFile f(make_consumer_json());
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("in").loop_timing, RoleConfig::LoopTimingPolicy::FixedPace);
}

TEST(ActorConfigLoopTiming, ConsumerCompensating)
{
    TmpJsonFile f(make_consumer_json(R"("loop_timing": "compensating", "timeout_ms": 500)"));
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("in").loop_timing, RoleConfig::LoopTimingPolicy::Compensating);
    EXPECT_EQ(cfg.roles.at("in").timeout_ms, 500);
}

// ============================================================================
// broker / broker_pubkey parsing
// ============================================================================

TEST(ActorConfigBroker, DefaultBrokerEndpoint)
{
    TmpJsonFile f(make_producer_json());
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("out").broker, "tcp://127.0.0.1:5570");
}

TEST(ActorConfigBroker, CustomBrokerEndpoint)
{
    TmpJsonFile f(make_producer_json(R"("broker": "tcp://10.0.0.1:9000")"));
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("out").broker, "tcp://10.0.0.1:9000");
}

TEST(ActorConfigBroker, DefaultBrokerPubkeyEmpty)
{
    TmpJsonFile f(make_producer_json());
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_TRUE(cfg.roles.at("out").broker_pubkey.empty());
}

TEST(ActorConfigBroker, BrokerPubkeyParsed)
{
    TmpJsonFile f(make_producer_json(
        R"("broker_pubkey": "abcde12345abcde12345abcde12345abcde12345")"));
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("out").broker_pubkey,
              "abcde12345abcde12345abcde12345abcde12345");
}

// ============================================================================
// interval_ms / timeout_ms
// ============================================================================

TEST(ActorConfigTiming, ProducerIntervalDefault)
{
    TmpJsonFile f(make_producer_json());
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("out").interval_ms, 0);
}

TEST(ActorConfigTiming, ProducerIntervalMs)
{
    TmpJsonFile f(make_producer_json(R"("interval_ms": 100)"));
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("out").interval_ms, 100);
}

TEST(ActorConfigTiming, ProducerIntervalTriggered)
{
    TmpJsonFile f(make_producer_json(R"("interval_ms": -1)"));
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("out").interval_ms, -1);
}

TEST(ActorConfigTiming, ConsumerTimeoutDefault)
{
    TmpJsonFile f(make_consumer_json());
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("in").timeout_ms, -1);
}

TEST(ActorConfigTiming, ConsumerTimeoutMs)
{
    TmpJsonFile f(make_consumer_json(R"("timeout_ms": 5000)"));
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.at("in").timeout_ms, 5000);
}

// ============================================================================
// ValidationPolicy parsing
// ============================================================================

TEST(ActorConfigValidation, DefaultPolicies)
{
    TmpJsonFile f(make_producer_json());
    const auto &v = ActorConfig::from_json_file(f.path()).roles.at("out").validation;
    EXPECT_EQ(v.slot_checksum,     ValidationPolicy::Checksum::Update);
    EXPECT_EQ(v.flexzone_checksum, ValidationPolicy::Checksum::Update);
    EXPECT_EQ(v.on_checksum_fail,  ValidationPolicy::OnFail::Skip);
    EXPECT_EQ(v.on_python_error,   ValidationPolicy::OnPyError::Continue);
}

TEST(ActorConfigValidation, AllFieldsParsed)
{
    TmpJsonFile f(make_producer_json(
        R"("validation": {)"
        R"(  "slot_checksum": "enforce",)"
        R"(  "flexzone_checksum": "none",)"
        R"(  "on_checksum_fail": "pass",)"
        R"(  "on_python_error": "stop")"
        R"(})"));
    const auto &v = ActorConfig::from_json_file(f.path()).roles.at("out").validation;
    EXPECT_EQ(v.slot_checksum,     ValidationPolicy::Checksum::Enforce);
    EXPECT_EQ(v.flexzone_checksum, ValidationPolicy::Checksum::None);
    EXPECT_EQ(v.on_checksum_fail,  ValidationPolicy::OnFail::Pass);
    EXPECT_EQ(v.on_python_error,   ValidationPolicy::OnPyError::Stop);
}

TEST(ActorConfigValidation, ChecksumNoneRoundTrip)
{
    TmpJsonFile f(make_producer_json(R"("validation": {"slot_checksum": "none"})"));
    const auto &v = ActorConfig::from_json_file(f.path()).roles.at("out").validation;
    EXPECT_EQ(v.slot_checksum, ValidationPolicy::Checksum::None);
}

TEST(ActorConfigValidation, InvalidChecksumThrows)
{
    TmpJsonFile f(make_producer_json(R"("validation": {"slot_checksum": "maybe"})"));
    EXPECT_THROW(ActorConfig::from_json_file(f.path()), std::runtime_error);
}

TEST(ActorConfigValidation, InvalidOnFailThrows)
{
    TmpJsonFile f(make_producer_json(R"("validation": {"on_checksum_fail": "ignore"})"));
    EXPECT_THROW(ActorConfig::from_json_file(f.path()), std::runtime_error);
}

TEST(ActorConfigValidation, InvalidOnPyErrorThrows)
{
    TmpJsonFile f(make_producer_json(R"("validation": {"on_python_error": "crash"})"));
    EXPECT_THROW(ActorConfig::from_json_file(f.path()), std::runtime_error);
}

// ============================================================================
// actor block and UID handling
// ============================================================================

TEST(ActorConfigActor, UidAndNameParsed)
{
    TmpJsonFile f(make_producer_json());
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.actor_uid,  "ACTOR-TEST-12345678");
    EXPECT_EQ(cfg.actor_name, "TestActor");
}

TEST(ActorConfigActor, LogLevelParsed)
{
    const std::string json =
        "{\n"
        "  \"script\": \"test.py\",\n"
        "  \"actor\": {\"uid\": \"ACTOR-T-00000001\", \"name\": \"T\","
        "              \"log_level\": \"debug\"},\n"
        "  \"roles\": {}\n"
        "}";
    TmpJsonFile f(json);
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.log_level, "debug");
}

TEST(ActorConfigActor, AbsentUidAutoGenerated)
{
    // uid absent → auto-generated with ACTOR- prefix.
    const std::string json =
        "{\n"
        "  \"script\": \"test.py\",\n"
        "  \"actor\": {\"name\": \"AutoGen\"},\n"
        "  \"roles\": {}\n"
        "}";
    TmpJsonFile f(json);
    const auto cfg = ActorConfig::from_json_file(f.path());
    ASSERT_GE(cfg.actor_uid.size(), 6u);
    EXPECT_EQ(cfg.actor_uid.substr(0, 6), "ACTOR-");
}

TEST(ActorConfigActor, NonConformingUidAccepted)
{
    // Non-ACTOR- prefix logs a warning (stderr) but does NOT throw.
    const std::string json =
        "{\n"
        "  \"script\": \"test.py\",\n"
        "  \"actor\": {\"uid\": \"custom-uid-999\", \"name\": \"Custom\"},\n"
        "  \"roles\": {}\n"
        "}";
    TmpJsonFile f(json);
    EXPECT_NO_THROW({
        const auto cfg = ActorConfig::from_json_file(f.path());
        EXPECT_EQ(cfg.actor_uid, "custom-uid-999");
    });
}

// ============================================================================
// Multi-role configs
// ============================================================================

TEST(ActorConfigMultiRole, TwoRolesParsed)
{
    const std::string json =
        "{\n"
        "  \"script\": \"test.py\",\n"
        "  \"actor\": {\"uid\": \"ACTOR-MULTI-00000001\", \"name\": \"Multi\"},\n"
        "  \"roles\": {\n"
        "    \"out\": {\"kind\": \"producer\", \"channel\": \"lab.data\"},\n"
        "    \"cfg\": {\"kind\": \"consumer\", \"channel\": \"lab.config\"}\n"
        "  }\n"
        "}";
    TmpJsonFile f(json);
    const auto cfg = ActorConfig::from_json_file(f.path());
    EXPECT_EQ(cfg.roles.size(), 2u);
    ASSERT_EQ(cfg.roles.count("out"), 1u);
    ASSERT_EQ(cfg.roles.count("cfg"), 1u);
    EXPECT_EQ(cfg.roles.at("out").kind, RoleConfig::Kind::Producer);
    EXPECT_EQ(cfg.roles.at("cfg").kind, RoleConfig::Kind::Consumer);
}

TEST(ActorConfigMultiRole, EmptyRolesMapAccepted)
{
    const std::string json =
        "{\n"
        "  \"script\": \"test.py\",\n"
        "  \"actor\": {\"uid\": \"ACTOR-EMPTY-00000001\"},\n"
        "  \"roles\": {}\n"
        "}";
    TmpJsonFile f(json);
    EXPECT_NO_THROW({
        const auto cfg = ActorConfig::from_json_file(f.path());
        EXPECT_TRUE(cfg.roles.empty());
    });
}

// ============================================================================
// Error cases
// ============================================================================

TEST(ActorConfigErrors, FileNotFoundThrows)
{
    EXPECT_THROW(
        ActorConfig::from_json_file("/nonexistent/path/actor_config_test.json"),
        std::runtime_error);
}

TEST(ActorConfigErrors, MalformedJsonThrows)
{
    TmpJsonFile f("{ this is not : valid json }");
    EXPECT_THROW(ActorConfig::from_json_file(f.path()), std::runtime_error);
}

TEST(ActorConfigErrors, MissingScriptThrows)
{
    const std::string json =
        "{\n"
        "  \"actor\": {\"uid\": \"ACTOR-T-00000001\"},\n"
        "  \"roles\": {}\n"
        "}";
    TmpJsonFile f(json);
    EXPECT_THROW(ActorConfig::from_json_file(f.path()), std::runtime_error);
}

TEST(ActorConfigErrors, MissingChannelInRoleThrows)
{
    const std::string json =
        "{\n"
        "  \"script\": \"test.py\",\n"
        "  \"roles\": {\"out\": {\"kind\": \"producer\"}}\n"
        "}";
    TmpJsonFile f(json);
    EXPECT_THROW(ActorConfig::from_json_file(f.path()), std::runtime_error);
}

TEST(ActorConfigErrors, InvalidRoleKindThrows)
{
    const std::string json =
        "{\n"
        "  \"script\": \"test.py\",\n"
        "  \"roles\": {\"out\": {\"kind\": \"relay\", \"channel\": \"lab.test\"}}\n"
        "}";
    TmpJsonFile f(json);
    EXPECT_THROW(ActorConfig::from_json_file(f.path()), std::runtime_error);
}
