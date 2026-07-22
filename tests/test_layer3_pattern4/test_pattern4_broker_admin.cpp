/**
 * @file test_pattern4_broker_admin.cpp
 * @brief Pattern 4 broker-admin wire tests (REG validation + channel
 *        introspection).
 *
 * Successors of the broker_admin workers formerly hosted under
 * `tests/test_layer3_datahub/workers/broker_admin_workers.cpp` against the
 * retired in-process HubHostBrokerHandle harness (task #52 sweep):
 *   - REG_REQ body-validation error paths (Round 2).
 *   - reg_validation_*_success — verified via DISC_REQ data_transport.
 *   - list_channels / snapshot — verified via the CHANNEL_LIST_REQ wire
 *     query (the broker's producer_pid is echoed on that ACK) instead of
 *     the in-process list_channels_json_str / query_channel_snapshot reads.
 *
 * Still in L3: close_channel_{existing,non_existent} — they trigger the
 * admin close via the in-process request_close_channel; the only wire path
 * is the AdminService socket (admin token), tracked as a follow-up.
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

class Pattern4BrokerAdminTest : public pylabhub::tests::pattern4::Pattern4WireTest
{
  protected:
    // Send a (possibly-mutated) REG_REQ body and return the reply.
    std::optional<nlohmann::json> reg(BrokerWireClient &c, const nlohmann::json &body)
    {
        return c.request("REG_REQ", body, "REG_ACK",
                         std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
    }

    // CHANNEL_LIST_REQ — the wire admin introspection query (replaces the
    // in-process list_channels_json_str / query_channel_snapshot).
    std::optional<nlohmann::json> channel_list(BrokerWireClient &c)
    {
        return c.request("CHANNEL_LIST_REQ", nlohmann::json::object(), "CHANNEL_LIST_ACK",
                         std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
    }

    // Find a channel entry by name in a CHANNEL_LIST_ACK body; nullptr if
    // absent.  `list` must outlive the returned pointer.
    static const nlohmann::json *find_channel(const nlohmann::json &list, const std::string &name)
    {
        if (!list.contains("channels") || !list.at("channels").is_array())
            return nullptr;
        for (const auto &ch : list.at("channels"))
            if (ch.value("name", std::string{}) == name)
                return &ch;
        return nullptr;
    }

    std::optional<nlohmann::json> discover(BrokerWireClient &c, const std::string &channel)
    {
        nlohmann::json body;
        body["channel_name"] = channel;
        return c.request("DISC_REQ", body, "DISC_ACK",
                         std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
    }
};

} // namespace

#define SPAWN_BROKER(temp_dir)                                                                     \
    SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker", {(temp_dir).string(), "default"})

// data_transport is REQUIRED on the ProducerRegReqBody wire class
// (HEP-CORE-0036 §5b.4 / HEP-CORE-0046 §14.3).  Missing → the wire body
// class ctor rejects with BODY_SCHEMA_VIOLATION.
TEST_F(Pattern4BrokerAdminTest, RegValidation_MissingDataTransport)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.missing_dt" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_missing_dt");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/true);
    body.erase("data_transport");
    auto resp = reg(prod, body);
    ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "BODY_SCHEMA_VIOLATION");
    EXPECT_NE(resp->value("message", std::string{}).find("data_transport"), std::string::npos)
        << "error message should name the missing field; body=" << resp->dump();

    broker.signal_quit();
}

// Explicit empty data_transport is a value error, not a schema error →
// INVALID_REQUEST ("is not one of {shm,zmq}").
TEST_F(Pattern4BrokerAdminTest, RegValidation_EmptyDataTransport)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.empty_dt" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_empty_dt");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.bogus_dt" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_bogus_dt");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/true);
    body["data_transport"] = "tcp";
    auto resp = reg(prod, body);
    ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST");
    EXPECT_NE(resp->value("message", std::string{}).find("tcp"), std::string::npos)
        << "error message should echo the bad value; body=" << resp->dump();

    broker.signal_quit();
}

// data_transport="shm" but no shm_capability_endpoint → INVALID_REQUEST
// (HEP-CORE-0041 §5.1 endpoint-required check).
TEST_F(Pattern4BrokerAdminTest, RegValidation_ShmMissingEndpoint)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.shm_no_ep" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_shm_no_ep");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/true);
    body.erase("shm_capability_endpoint"); // data_transport stays "shm"
    auto resp = reg(prod, body);
    ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST");
    EXPECT_NE(resp->value("message", std::string{}).find("shm_capability_endpoint"),
              std::string::npos)
        << "error message should name the missing endpoint; body=" << resp->dump();

    broker.signal_quit();
}

// ─── CHANNEL_LIST_REQ introspection (replaces list_channels_json_str /
//     query_channel_snapshot in-process reads) ────────────────────────────

TEST_F(Pattern4BrokerAdminTest, ListChannels_Empty)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string querier = "prod.admin.list_empty" + suffix;

    const fs::path temp_dir = make_test_temp_dir("ba_list_empty");
    const auto setup = make_pattern4_setup({querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto c = make_wire_client(ctx, setup, querier);
    auto list = channel_list(c);
    ASSERT_TRUE(list.has_value()) << "CHANNEL_LIST_REQ timed out";
    ASSERT_TRUE(list->contains("channels") && list->at("channels").is_array());
    EXPECT_TRUE(list->at("channels").empty())
        << "expected empty channel list; body=" << list->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerAdminTest, ListChannels_OneChannel)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "admin.list.one" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_list_one");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));

    auto list = channel_list(prod);
    ASSERT_TRUE(list.has_value());
    EXPECT_NE(find_channel(*list, channel), nullptr)
        << "channel absent from CHANNEL_LIST_ACK; body=" << list->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerAdminTest, ListChannels_FieldPresence)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "admin.list.fields" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_list_fields");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));

    auto list = channel_list(prod);
    ASSERT_TRUE(list.has_value());
    const auto *entry = find_channel(*list, channel);
    ASSERT_NE(entry, nullptr) << "channel not found; body=" << list->dump();
    EXPECT_TRUE(entry->contains("name"));
    EXPECT_TRUE(entry->contains("observable"));
    EXPECT_TRUE(entry->contains("consumer_count"));
    EXPECT_TRUE(entry->contains("producer_pid"))
        << "CHANNEL_LIST_ACK entry must carry producer_pid; entry=" << entry->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerAdminTest, Snapshot_Empty)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string querier = "prod.admin.snap_empty" + suffix;

    const fs::path temp_dir = make_test_temp_dir("ba_snap_empty");
    const auto setup = make_pattern4_setup({querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto c = make_wire_client(ctx, setup, querier);
    auto list = channel_list(c);
    ASSERT_TRUE(list.has_value());
    EXPECT_TRUE(list->at("channels").empty());

    broker.signal_quit();
}

TEST_F(Pattern4BrokerAdminTest, Snapshot_OneChannel)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "admin.snap.one" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_snap_one");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));

    auto list = channel_list(prod);
    ASSERT_TRUE(list.has_value());
    const auto *entry = find_channel(*list, channel);
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->value("observable", std::string{}).empty());
    EXPECT_EQ(entry->value("consumer_count", 99), 0);

    broker.signal_quit();
}

TEST_F(Pattern4BrokerAdminTest, Snapshot_AfterConsumer)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "admin.snap.consumer" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_snap_consumer");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    auto list = channel_list(prod);
    ASSERT_TRUE(list.has_value());
    const auto *entry = find_channel(*list, channel);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->value("consumer_count", 99), 1);

    broker.signal_quit();
}

// ─── reg_validation success paths — verified via DISC data_transport ───────

TEST_F(Pattern4BrokerAdminTest, RegValidation_ShmSuccess)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.shm_ok" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_shm_ok");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    auto r = reg(prod, producer_reg_body(setup, channel, uid, /*shm=*/true));
    ASSERT_TRUE(r.has_value()) << "REG_REQ timed out";
    ASSERT_EQ(r->value("status", std::string{}), "success") << r->dump();
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));

    auto disc = discover(prod, channel);
    ASSERT_TRUE(disc.has_value());
    EXPECT_EQ(disc->value("data_transport", std::string{}), "shm") << "body=" << disc->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerAdminTest, RegValidation_ZmqSuccess)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "reg.val.zmq_ok" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("ba_zmq_ok");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));

    auto disc = discover(prod, channel);
    ASSERT_TRUE(disc.has_value());
    EXPECT_EQ(disc->value("data_transport", std::string{}), "zmq") << "body=" << disc->dump();

    broker.signal_quit();
}
