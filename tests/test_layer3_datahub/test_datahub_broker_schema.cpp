/**
 * @file test_datahub_broker_schema.cpp
 * @brief Broker named schema protocol tests (HEP-CORE-0016 Phase 3).
 *
 * Suite: BrokerSchemaTest
 *
 * Tests the broker SCHEMA_REQ/SCHEMA_ACK round-trip and the schema_id/blds/hash
 * annotation stored by the broker at REG_REQ time.  Consumer schema_id validation
 * (expected_schema_id in CONSUMER_REG_REQ) is also exercised.
 *
 * All tests use an in-process broker (LocalBrokerHandle pattern from
 * test_datahub_channel_access_policy.cpp) with schema_search_dirs empty
 * so the library is always empty — no schema files needed on disk.
 *
 * CTest safety: all channel names are suffixed with the test process PID.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/messenger.hpp"

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;

namespace
{

// ── LocalBrokerHandle — in-process broker (same pattern as access-policy tests) ─

struct LocalBrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;
    std::string                    pubkey;

    LocalBrokerHandle() = default;
    LocalBrokerHandle(LocalBrokerHandle&&) noexcept = default;
    LocalBrokerHandle& operator=(LocalBrokerHandle&&) noexcept = default;
    ~LocalBrokerHandle() { stop_and_join(); }

    void stop_and_join()
    {
        if (service)
        {
            service->stop();
            if (thread.joinable())
                thread.join();
        }
    }
};

LocalBrokerHandle start_local_broker(BrokerService::Config cfg)
{
    using ReadyInfo = std::pair<std::string, std::string>;
    auto promise = std::make_shared<std::promise<ReadyInfo>>();
    auto future  = promise->get_future();

    cfg.on_ready = [promise](const std::string& ep, const std::string& pk)
    { promise->set_value({ep, pk}); };

    auto svc     = std::make_unique<BrokerService>(std::move(cfg));
    auto raw_ptr = svc.get();
    std::thread t([raw_ptr] { raw_ptr->run(); });

    auto info = future.get();

    LocalBrokerHandle h;
    h.service  = std::move(svc);
    h.thread   = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

// Suffix channel name with PID for ctest -j safety.
std::string pid_chan(const std::string& base)
{
    return base + "." + std::to_string(getpid());
}

// 32-byte raw binary hash for create_channel() (which hex-encodes it for the broker).
// 0xAA repeated 32 times → hex-encoded = "aa" × 32 = 64 'a' chars.
const std::string kTestHashRaw(32, '\xaa');
// Expected hex encoding after the round-trip through the broker.
const std::string kTestHashHex(64, 'a');

} // anonymous namespace

// ============================================================================
// BrokerSchemaTest — in-process broker, empty schema library
// ============================================================================

class BrokerSchemaTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                pylabhub::crypto::GetLifecycleModule(),
                pylabhub::hub::GetLifecycleModule()), std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

  protected:
    void SetUp() override
    {
        BrokerService::Config cfg;
        cfg.endpoint           = "tcp://127.0.0.1:0"; // ephemeral port
        cfg.schema_search_dirs = {};                   // empty = no library files
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string& ep() const { return broker_->endpoint; }
    const std::string& pk() const { return broker_->pubkey;   }

    std::optional<LocalBrokerHandle> broker_;

  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<pylabhub::utils::LifecycleGuard> BrokerSchemaTest::s_lifecycle_;

// ── Test 1: schema_id stored and returned via query_channel_schema ─────────────

TEST_F(BrokerSchemaTest, SchemaId_StoredOnReg)
{
    // Producer registers channel with schema_id="lab.test.sensor@1".
    // Since the library is empty, the broker stores the ID as-is (Case A library miss).
    // query_channel_schema must echo it back.
    const std::string channel = pid_chan("schema.id.stored");
    const std::string schema_id = "lab.test.sensor@1";

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));

    auto handle = m.create_channel(channel, {.timeout_ms = 3000, .schema_id = schema_id});
    ASSERT_TRUE(handle.has_value()) << "create_channel failed";

    auto info = m.query_channel_schema(channel, 3000);
    ASSERT_TRUE(info.has_value()) << "query_channel_schema returned nullopt";
    EXPECT_EQ(info->schema_id, schema_id);
}

// ── Test 2: schema_blds stored and returned via query_channel_schema ───────────

TEST_F(BrokerSchemaTest, SchemaBlds_StoredOnReg)
{
    const std::string channel   = pid_chan("schema.blds.stored");
    const std::string blds      = "B1|u64:ts,f64:temperature";

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));

    auto handle = m.create_channel(channel,
                                   {.timeout_ms = 3000, .schema_id = "lab.temp@1",
                                    .schema_blds = blds});
    ASSERT_TRUE(handle.has_value());

    auto info = m.query_channel_schema(channel, 3000);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->blds, blds);
}

// ── Test 3: schema_hash returned verbatim via query_channel_schema ─────────────

TEST_F(BrokerSchemaTest, SchemaHash_ReturnedOnQuery)
{
    const std::string channel = pid_chan("schema.hash.query");

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));

    auto handle = m.create_channel(channel,
                                   {.schema_hash = kTestHashRaw, .timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    auto info = m.query_channel_schema(channel, 3000);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->hash_hex, kTestHashHex);
}

// ── Test 4: query_channel_schema for unknown channel returns nullopt ───────────

TEST_F(BrokerSchemaTest, SchemaReq_UnknownChannel_ReturnsNullopt)
{
    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));

    auto info = m.query_channel_schema(pid_chan("schema.nonexistent"), 2000);
    EXPECT_FALSE(info.has_value())
        << "Expected nullopt for unregistered channel, got schema_id=" << info->schema_id;
}

// ── Test 5: consumer expected_schema_id matches producer's schema_id → success ─

TEST_F(BrokerSchemaTest, ConsumerSchemaId_IdMatch_Succeeds)
{
    const std::string channel   = pid_chan("schema.consumer.match");
    const std::string schema_id = "lab.consumer.test@2";

    // Producer registers with schema_id.
    Messenger producer_m;
    ASSERT_TRUE(producer_m.connect(ep(), pk()));
    auto prod_handle = producer_m.create_channel(channel,
                                                  {.timeout_ms = 3000, .schema_id = schema_id});
    ASSERT_TRUE(prod_handle.has_value()) << "Producer create_channel failed";

    // Consumer expects the same schema_id → CONSUMER_REG_REQ should succeed.
    Messenger consumer_m;
    ASSERT_TRUE(consumer_m.connect(ep(), pk()));
    auto cons_handle = consumer_m.connect_channel(channel,
                                                   /*timeout_ms=*/3000,
                                                   /*schema_hash=*/{},
                                                   /*consumer_uid=*/{},
                                                   /*consumer_name=*/{},
                                                   /*expected_schema_id=*/schema_id);
    EXPECT_TRUE(cons_handle.has_value())
        << "Consumer connect_channel should succeed when schema_id matches";
}

// ── Test 6: consumer expected_schema_id mismatch → connect fails ───────────────

TEST_F(BrokerSchemaTest, ConsumerSchemaId_Mismatch_Fails)
{
    const std::string channel    = pid_chan("schema.consumer.mismatch");
    const std::string prod_sid   = "lab.producer.schema@1";
    const std::string cons_sid   = "lab.other.schema@1"; // different; not in empty library

    Messenger producer_m;
    ASSERT_TRUE(producer_m.connect(ep(), pk()));
    auto prod_handle = producer_m.create_channel(channel,
                                                  {.timeout_ms = 3000, .schema_id = prod_sid});
    ASSERT_TRUE(prod_handle.has_value());

    // Consumer expects a different schema_id that is not in the empty library.
    // Broker → SCHEMA_ID_UNKNOWN → connect_channel returns nullopt.
    Messenger consumer_m;
    ASSERT_TRUE(consumer_m.connect(ep(), pk()));
    auto cons_handle = consumer_m.connect_channel(channel,
                                                   /*timeout_ms=*/3000,
                                                   /*schema_hash=*/{},
                                                   /*consumer_uid=*/{},
                                                   /*consumer_name=*/{},
                                                   /*expected_schema_id=*/cons_sid);
    EXPECT_FALSE(cons_handle.has_value())
        << "Consumer connect_channel should fail on schema_id mismatch";
}

// ── Test 7: producer anonymous; consumer expects schema_id → fail ──────────────

TEST_F(BrokerSchemaTest, ConsumerSchemaId_EmptyProducer_Fails)
{
    const std::string channel  = pid_chan("schema.consumer.empty.prod");
    const std::string cons_sid = "lab.expected.schema@3"; // not in empty library

    // Producer registers with no schema_id (anonymous).
    Messenger producer_m;
    ASSERT_TRUE(producer_m.connect(ep(), pk()));
    auto prod_handle = producer_m.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    // Consumer expects a schema_id but the channel has none, and the library is empty.
    // Broker → SCHEMA_ID_UNKNOWN → connect_channel returns nullopt.
    Messenger consumer_m;
    ASSERT_TRUE(consumer_m.connect(ep(), pk()));
    auto cons_handle = consumer_m.connect_channel(channel,
                                                   /*timeout_ms=*/3000,
                                                   /*schema_hash=*/{},
                                                   /*consumer_uid=*/{},
                                                   /*consumer_name=*/{},
                                                   /*expected_schema_id=*/cons_sid);
    EXPECT_FALSE(cons_handle.has_value())
        << "Consumer connect_channel should fail when producer is anonymous and schema_id expected";
}
