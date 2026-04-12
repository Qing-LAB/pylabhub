/**
 * @file test_datahub_hub_api.cpp
 * @brief Hub Producer/Consumer unified active-service API tests.
 *
 * Tests Producer and Consumer active services including:
 *  - Factory creation (template and non-template)
 *  - Active mode threads (peer_thread, write_thread, data_thread)
 *  - HELLO/BYE consumer tracking
 *  - ZMQ send/recv callbacks via on_zmq_data
 *  - SHM synced_write/push/pull/set_read_handler slot-processor API
 *  - ManagedProducer lifecycle
 *  - Secret mismatch: SHM detaches, ZMQ still works
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include <gtest/gtest.h>

#include <cstdio>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubHubApiTest : public IsolatedProcessTest
{
};

TEST_F(DatahubHubApiTest, ProducerCreatePubSub)
{
    // Producer::create(PubSub, no shm): is_valid, channel_name, close().
    auto proc = SpawnWorker("hub_api.producer_create_pubsub", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ProducerCreateWithShm)
{
    // Producer::create(has_shm): synced_write(job) executes sync slot;
    // push(job) queued and executed asynchronously by write_thread.
    auto proc = SpawnWorker("hub_api.producer_create_with_shm", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, NonTemplateFactory)
{
    // Non-template create/connect (no schema); factory returns valid Producer/Consumer.
    auto proc = SpawnWorker("hub_api.non_template_factory", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ManagedProducerLifecycle)
{
    // Producer lifecycle: start()/stop()/close() idempotent; is_running() correct.
    auto proc = SpawnWorker("hub_api.managed_producer_lifecycle", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ConsumerShmReadE2E)
{
    // Producer push (async) → consumer set_read_handler fires with correct data.
    auto proc = SpawnWorker("hub_api.consumer_shm_read_e2e", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ConsumerReadShmSync)
{
    // Synchronous SHM round-trip: synced_write then pull verify data fidelity.
    auto proc = SpawnWorker("hub_api.consumer_read_shm_sync", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ProducerConsumerIdempotency)
{
    // start()/stop()/close() each called twice is safe: no crash, correct return values.
    auto proc = SpawnWorker("hub_api.producer_consumer_idempotency", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ProducerChannelIdentity)
{
    // DataBlockConfig identity fields are written to SHM header at producer creation;
    // hub_uid/hub_name/producer_uid/producer_name are readable from both producer and consumer.
    auto proc = SpawnWorker("hub_api.producer_channel_identity", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ConsumerIdentityInShm)
{
    // ConsumerOptions::consumer_uid/consumer_name propagate into the heartbeat slot;
    // DataBlockConsumer::consumer_uid()/consumer_name() return those values.
    auto proc = SpawnWorker("hub_api.consumer_identity_in_shm", {});
    ExpectWorkerOk(proc);
}

// ── Queue abstraction forwarding API tests (Phase 4) ─────────────────────────

TEST_F(DatahubHubApiTest, ProducerConsumerForwardingApi)
{
    // write_acquire/commit through Producer, read_acquire/release through Consumer.
    auto proc = SpawnWorker("hub_api.producer_consumer_forwarding_api", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ConstructionTimeChecksum)
{
    // update_checksum in ProducerOptions → verify_checksum in ConsumerOptions;
    // checksum stamped at write_commit, verified at read_acquire — no post-creation setters.
    auto proc = SpawnWorker("hub_api.construction_time_checksum", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, FlexzoneThroughServiceLayer)
{
    // write_flexzone/read_flexzone through Producer/Consumer (DataBlock-direct path).
    auto proc = SpawnWorker("hub_api.flexzone_through_service_layer", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, QueueMetricsForwarding)
{
    // queue_metrics() + queue_policy_info() through Producer forwarding.
    auto proc = SpawnWorker("hub_api.queue_metrics_forwarding", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ZmqForwardingApi)
{
    // ZMQ transport with ephemeral port: write/read through forwarding; endpoint update.
    auto proc = SpawnWorker("hub_api.zmq_forwarding_api", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, RuntimeVerifyChecksumToggle)
{
    // Runtime toggle: write without checksum, enable verify, write with checksum, verify passes.
    auto proc = SpawnWorker("hub_api.runtime_verify_checksum_toggle", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ForwardingErrorPaths)
{
    // item_size=0: forwarding returns nullptr/0/false safely, no crash.
    auto proc = SpawnWorker("hub_api.forwarding_error_paths", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ChecksumEnforcedRoundtrip)
{
    // Enforced: Producer stamps checksum, Consumer verifies — round-trip succeeds.
    auto proc = SpawnWorker("hub_api.checksum_enforced_roundtrip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ChecksumManualNoStampMismatch)
{
    // Manual: Producer does not stamp, Consumer verifies — read_acquire returns nullptr.
    auto proc = SpawnWorker("hub_api.checksum_manual_no_stamp_mismatch", {});
    ExpectWorkerOk(proc, {}, {"slot checksum error"});
}

TEST_F(DatahubHubApiTest, ChecksumNoneRoundtrip)
{
    // None: no checksum at all — round-trip succeeds.
    auto proc = SpawnWorker("hub_api.checksum_none_roundtrip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, OpenInboxClientNumericSchema)
{
    // Full broker path: register producer with numeric-only inbox schema,
    // open_inbox_client connects and returns correct item_size.
    auto proc = SpawnWorker("hub_api.open_inbox_client_numeric_schema", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ShmMultifieldSchemaRoundtrip)
{
    // Multi-type schema (float64+int32+uint8) through SHM: write all fields, read all fields.
    auto proc = SpawnWorker("hub_api.shm_multifield_schema_roundtrip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ZmqMultifieldSchemaRoundtrip)
{
    // Multi-type schema (float64+float32[3]+uint16+bytes[5]+string[16]+int64) through ZMQ.
    auto proc = SpawnWorker("hub_api.zmq_multifield_schema_roundtrip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ShmMultifieldPackedRoundtrip)
{
    // Same 6-field schema with packed packing — no alignment padding.
    // Verifies packed size < aligned size and data survives packed layout.
    auto proc = SpawnWorker("hub_api.shm_multifield_packed_roundtrip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ZmqMultifieldPackedRoundtrip)
{
    // Same 6-field schema with packed packing through ZMQ wire format.
    auto proc = SpawnWorker("hub_api.zmq_multifield_packed_roundtrip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ChecksumEnforcedComplexSchema)
{
    // Enforced checksum with 6-field complex schema — stamp + verify round-trip.
    auto proc = SpawnWorker("hub_api.checksum_enforced_complex_schema", {});
    ExpectWorkerOk(proc);
}

// Helper: extract a uint64_t value from stderr output matching "KEY=<value>"
static uint64_t extract_ts(std::string_view stderr_out, const char *key)
{
    std::string prefix = std::string(key) + "=";
    auto pos = stderr_out.find(prefix);
    EXPECT_NE(pos, std::string_view::npos) << "Missing " << key << " in stderr output";
    if (pos == std::string_view::npos) return 0;
    auto val_start = pos + prefix.size();
    auto val_end = stderr_out.find('\n', val_start);
    return std::stoull(std::string(stderr_out.substr(val_start, val_end - val_start)));
}

// Helper: extract a string value from stderr output matching "KEY=<value>"
static std::string extract_str(std::string_view stderr_out, const char *key)
{
    std::string prefix = std::string(key) + "=";
    auto pos = stderr_out.find(prefix);
    EXPECT_NE(pos, std::string_view::npos) << "Missing " << key << " in stderr output";
    if (pos == std::string_view::npos) return {};
    auto val_start = pos + prefix.size();
    auto val_end = stderr_out.find('\n', val_start);
    return std::string(stderr_out.substr(val_start, val_end - val_start));
}

TEST_F(DatahubHubApiTest, SpinlockThroughRoleApi)
{
    // Multi-process spinlock test through RoleAPIBase:
    //   Process 1 (producer): starts broker, creates Producer (SHM), wires RoleAPIBase,
    //     acquires spinlock 0, signals ready, waits for consumer rendezvous, releases.
    //   Process 2 (consumer): connects to same broker, creates Consumer (SHM attachment),
    //     wires RoleAPIBase, signals rendezvous, calls try_lock_for(-1), verifies blocking.

    std::string channel = make_test_channel_name("hub.spinlock_api");

    // Spawn producer with ready signal -- it holds spinlock 0 after signaling
    auto producer = SpawnWorkerWithReadySignal(
        "hub_api.spinlock_producer_hold", {channel});
    producer.wait_for_ready();

    // Parse broker coordinates from producer stderr
    std::string broker_ep = extract_str(producer.get_stderr(), "BROKER_EP");
    std::string broker_pk = extract_str(producer.get_stderr(), "BROKER_PK");
    ASSERT_FALSE(broker_ep.empty()) << "Producer must print BROKER_EP";
    ASSERT_FALSE(broker_pk.empty()) << "Producer must print BROKER_PK";

    // Spawn consumer -- connects to same broker, contends on same spinlock
    auto consumer = SpawnWorker(
        "hub_api.spinlock_consumer_contend",
        {channel, broker_ep, broker_pk});

    consumer.wait_for_exit();
    producer.wait_for_exit();

    // Verify producer event sequence via required substrings
    expect_worker_ok(producer, {
        "[PRODUCER] spinlock[0] ACQUIRED",
        "[PRODUCER] spinlock[1] held by consumer",
        "[PRODUCER] spinlock[0] RELEASED",
        "[PRODUCER] consumer done"
    }, {
        // Expected ERRORs: out-of-range spinlock index test generates one
        "Initialized with a null SharedSpinLockState"
    });

    // Verify consumer event sequence via required substrings
    expect_worker_ok(consumer, {
        "[CONSUMER] spinlock[1] ACQUIRED",
        "[CONSUMER] trying spinlock[0]",
        "[CONSUMER] spinlock[0] ACQUIRED",
        "[CONSUMER] spinlock[0] RELEASED",
        "[CONSUMER] spinlock[2] independently acquirable",
        "[CONSUMER] spinlock[1] RELEASED"
    }, {
        "Initialized with a null SharedSpinLockState"
    });

    // Parse timestamps and verify cross-process mutual exclusion
    uint64_t t1_try      = extract_ts(consumer.get_stderr(), "TRY_TS");
    uint64_t t2_release  = extract_ts(producer.get_stderr(), "RELEASE_TS");
    uint64_t t3_acquired = extract_ts(consumer.get_stderr(), "ACQUIRED_TS");

    EXPECT_LT(t1_try, t2_release)
        << "Consumer must have started trying (T1) before producer released (T2)";
    EXPECT_LE(t2_release, t3_acquired)
        << "Consumer must have acquired (T3) after producer released (T2)";

    // Consumer was blocked for a meaningful duration
    uint64_t blocked_ns = t3_acquired - t1_try;
    constexpr uint64_t kMinBlockedNs = 20'000'000; // 20ms minimum
    EXPECT_GE(blocked_ns, kMinBlockedNs)
        << "Consumer should have been blocked >= 20ms, but only blocked "
        << (blocked_ns / 1'000'000) << "ms";
}
