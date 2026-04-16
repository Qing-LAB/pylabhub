/**
 * @file test_datahub_role_flexzone.cpp
 * @brief L3 tests for flexzone access through RoleAPIBase.
 *
 * Tests T2/T3 from flexzone_api_design.md: verifies that api.flexzone(side)
 * returns a valid pointer backed by real SHM, and that the Tx and Rx sides
 * of the same channel see the same physical region.
 *
 * Uses RoleAPIBase::build_tx_queue / build_rx_queue — the role API layer,
 * not bare-metal ShmQueue. LifecycleGuard provides Logger + Crypto + ZMQContext.
 */
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/zmq_context.hpp"
#include "utils/schema_utils.hpp"
#include "test_schema_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <source_location>
#include <string>

using namespace pylabhub;
using namespace pylabhub::scripting;

// ============================================================================
// Test fixture
// ============================================================================

class RoleFlexzoneTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<utils::LifecycleGuard>(
            utils::MakeModDefList(
                utils::Logger::GetLifecycleModule(),
                crypto::GetLifecycleModule(),
                hub::GetZMQContextModule(),
                hub::GetDataBlockModule()),
            std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

  private:
    static std::unique_ptr<utils::LifecycleGuard> s_lifecycle_;
};

std::unique_ptr<utils::LifecycleGuard> RoleFlexzoneTest::s_lifecycle_;

// ============================================================================
// Helpers
// ============================================================================

static hub::ProducerOptions make_producer_opts(
    const std::string &channel, const hub::SchemaSpec &slot_spec,
    const hub::SchemaSpec &fz_spec, uint64_t secret)
{
    hub::ProducerOptions opts;
    opts.channel_name = channel;
    opts.has_shm = true;
    opts.shm_config.shared_secret        = secret;
    opts.shm_config.ring_buffer_capacity  = 4;
    opts.shm_config.physical_page_size    = hub::system_page_size();
    opts.shm_config.policy                = hub::DataBlockPolicy::RingBuffer;
    opts.shm_config.consumer_sync_policy  = hub::ConsumerSyncPolicy::Sequential;
    opts.shm_config.checksum_policy       = hub::ChecksumPolicy::None;
    opts.zmq_schema   = hub::schema_spec_to_zmq_fields(slot_spec);
    opts.zmq_packing  = "aligned";
    opts.fz_schema    = hub::schema_spec_to_zmq_fields(fz_spec);
    opts.fz_packing   = "aligned";
    opts.instance_id  = "test:fz-prod:tx";
    return opts;
}

static hub::ConsumerOptions make_consumer_opts(
    const std::string &channel, const hub::SchemaSpec &slot_spec,
    uint64_t secret)
{
    hub::ConsumerOptions opts;
    opts.channel_name = channel;
    opts.shm_name = channel;
    opts.shm_shared_secret = secret;
    opts.zmq_schema  = hub::schema_spec_to_zmq_fields(slot_spec);
    opts.zmq_packing = "aligned";
    opts.instance_id = "test:fz-cons:rx";
    return opts;
}

// ============================================================================
// T2 — Producer writes flexzone, consumer reads same data
// ============================================================================

TEST_F(RoleFlexzoneTest, ProducerConsumer_FlexzoneRoundTrip)
{
    auto slot_spec = pylabhub::tests::simple_schema();
    auto fz_spec   = pylabhub::tests::simple_schema();

    const std::string channel = "test.fz.roundtrip." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t secret = 0xDEAD'BEEF'CAFE'1234;

    // ── Producer side (Tx) ──────────────────────────────────────────────
    RoleHostCore prod_core;
    prod_core.set_out_fz_spec(hub::SchemaSpec{fz_spec},
        hub::align_to_physical_page(hub::compute_schema_size(fz_spec, "aligned")));

    auto prod_api = std::make_unique<RoleAPIBase>(prod_core, "prod", "PROD-FZ-TEST");
    ASSERT_TRUE(prod_api->build_tx_queue(make_producer_opts(channel, slot_spec, fz_spec, secret)));
    ASSERT_TRUE(prod_api->start_tx_queue());

    // Flexzone must be non-null with correct size.
    void *tx_fz = prod_api->flexzone(ChannelSide::Tx);
    ASSERT_NE(tx_fz, nullptr) << "Producer Tx flexzone must be non-null for SHM queue";
    EXPECT_GT(prod_api->flexzone_size(ChannelSide::Tx), 0u);
    EXPECT_TRUE(prod_api->tx_has_shm());

    // Write a sentinel to the Tx flexzone.
    const float sentinel = 42.5f;
    std::memcpy(tx_fz, &sentinel, sizeof(sentinel));

    // Commit one slot so the consumer can attach.
    void *slot = prod_api->write_acquire(std::chrono::milliseconds{500});
    ASSERT_NE(slot, nullptr);
    prod_api->write_commit();

    // ── Consumer side (Rx) ──────────────────────────────────────────────
    RoleHostCore cons_core;
    cons_core.set_in_fz_spec(hub::SchemaSpec{fz_spec},
        hub::align_to_physical_page(hub::compute_schema_size(fz_spec, "aligned")));

    auto cons_api = std::make_unique<RoleAPIBase>(cons_core, "cons", "CONS-FZ-TEST");
    ASSERT_TRUE(cons_api->build_rx_queue(make_consumer_opts(channel, slot_spec, secret)));
    ASSERT_TRUE(cons_api->start_rx_queue());

    // Consumer Rx flexzone must be non-null.
    void *rx_fz = cons_api->flexzone(ChannelSide::Rx);
    ASSERT_NE(rx_fz, nullptr) << "Consumer Rx flexzone must be non-null for SHM queue";

    // Same physical SHM region → same data.
    float read_back = 0.0f;
    std::memcpy(&read_back, rx_fz, sizeof(read_back));
    EXPECT_FLOAT_EQ(read_back, sentinel)
        << "Consumer Rx flexzone must see producer's written sentinel";

    // ── T3 — Consumer writes back, producer reads ───────────────────────
    const float ack = 99.9f;
    std::memcpy(rx_fz, &ack, sizeof(ack));

    float prod_read = 0.0f;
    std::memcpy(&prod_read, tx_fz, sizeof(prod_read));
    EXPECT_FLOAT_EQ(prod_read, ack)
        << "Producer Tx flexzone must see consumer's ack (bidirectional)";

    // Cleanup
    cons_api->close_queues();
    prod_api->close_queues();
}

// ============================================================================
// T2b — api.flexzone() returns nullptr for ZMQ-only (no SHM)
// ============================================================================

TEST_F(RoleFlexzoneTest, ZmqOnly_FlexzoneIsNull)
{
    RoleHostCore core;
    auto api = std::make_unique<RoleAPIBase>(core, "prod", "PROD-ZMQ-FZ");

    hub::ProducerOptions opts;
    opts.channel_name = "test.fz.zmq";
    opts.has_shm = false;
    opts.data_transport = "zmq";
    opts.zmq_node_endpoint = "tcp://127.0.0.1:0";
    opts.zmq_bind = true;
    auto spec = pylabhub::tests::simple_schema();
    opts.zmq_schema = hub::schema_spec_to_zmq_fields(spec);
    opts.zmq_packing = "aligned";
    opts.instance_id = "test:fz-zmq:tx";

    ASSERT_TRUE(api->build_tx_queue(opts));
    ASSERT_TRUE(api->start_tx_queue());

    EXPECT_EQ(api->flexzone(ChannelSide::Tx), nullptr);
    EXPECT_EQ(api->flexzone_size(ChannelSide::Tx), 0u);
    EXPECT_FALSE(api->tx_has_shm());

    api->close_queues();
}
