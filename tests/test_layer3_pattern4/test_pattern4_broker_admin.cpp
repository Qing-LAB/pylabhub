/**
 * @file test_pattern4_broker_admin.cpp
 * @brief Pattern 4 REG_REQ body-validation wire tests.
 *
 * Successors of the wire-only reg_validation workers formerly hosted
 * under `tests/test_layer3_datahub/workers/broker_admin_workers.cpp`
 * against the retired in-process HubHostBrokerHandle harness (task #52
 * sweep).  The other broker_admin workers (list_channels / snapshot /
 * close_channel / reg_validation_*_success) inspect in-process broker
 * state via `broker.service()` and stay in L3 for a Round-3 disposition.
 *
 * Broker runs in its own subprocess (the generic
 * `pattern4_broker_protocol.broker`); the parent drives wire traffic via
 * BrokerWireClient using the shared Pattern4WireTest base.
 */
#include "pattern4_wire_test_base.h"

#include "broker_wire_client.h"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::pattern4::BrokerWireClient;
using pylabhub::tests::pattern4::expect_log;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

class Pattern4BrokerAdminTest
    : public pylabhub::tests::pattern4::Pattern4WireTest
{
protected:
    // Send a (possibly-mutated) REG_REQ body and return the reply.
    std::optional<nlohmann::json> reg(BrokerWireClient     &c,
                                      const nlohmann::json &body)
    {
        return c.request("REG_REQ", body, "REG_ACK",
                         std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
    }
};

}  // namespace

#define SPAWN_BROKER(temp_dir)                                                  \
    SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",               \
                              {(temp_dir).string(), "default"})

// data_transport is REQUIRED on the ProducerRegReqBody wire class
// (HEP-CORE-0036 §5b.4 / HEP-CORE-0046 §14.3).  Missing → the wire body
// class ctor rejects with BODY_SCHEMA_VIOLATION.
TEST_F(Pattern4BrokerAdminTest, RegValidation_MissingDataTransport)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.missing_dt" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_missing_dt");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/true);
    body.erase("data_transport");
    auto resp = reg(prod, body);
    ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "BODY_SCHEMA_VIOLATION");
    EXPECT_NE(resp->value("message", std::string{}).find("data_transport"),
              std::string::npos)
        << "error message should name the missing field; body=" << resp->dump();

    broker.signal_quit();
}

// Explicit empty data_transport is a value error, not a schema error →
// INVALID_REQUEST ("is not one of {shm,zmq}").
TEST_F(Pattern4BrokerAdminTest, RegValidation_EmptyDataTransport)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.empty_dt" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_empty_dt");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/true);
    body["data_transport"] = "";
    auto resp = reg(prod, body);
    ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST")
        << "body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerAdminTest, RegValidation_BogusDataTransport)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.bogus_dt" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_bogus_dt");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/true);
    body["data_transport"] = "tcp";
    auto resp = reg(prod, body);
    ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST");
    EXPECT_NE(resp->value("message", std::string{}).find("tcp"),
              std::string::npos)
        << "error message should echo the bad value; body=" << resp->dump();

    broker.signal_quit();
}

// data_transport="shm" but no shm_capability_endpoint → INVALID_REQUEST
// (HEP-CORE-0041 §5.1 endpoint-required check).
TEST_F(Pattern4BrokerAdminTest, RegValidation_ShmMissingEndpoint)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.shm_no_ep" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_shm_no_ep");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/true);
    body.erase("shm_capability_endpoint");  // data_transport stays "shm"
    auto resp = reg(prod, body);
    ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST");
    EXPECT_NE(resp->value("message", std::string{}).find("shm_capability_endpoint"),
              std::string::npos)
        << "error message should name the missing endpoint; body=" << resp->dump();

    broker.signal_quit();
}
