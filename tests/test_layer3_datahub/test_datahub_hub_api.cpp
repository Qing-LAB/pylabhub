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
#include <gtest/gtest.h>

using namespace pylabhub::tests;

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

TEST_F(DatahubHubApiTest, ConsumerConnect)
{
    // Producer + Consumer via unified API; ZMQ send/recv end-to-end.
    auto proc = SpawnWorker("hub_api.consumer_connect_e2e", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ConsumerHelloTracked)
{
    // After Consumer::connect(), producer.connected_consumers() contains the identity.
    auto proc = SpawnWorker("hub_api.consumer_hello_tracked", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ActiveProducerConsumerCallbacks)
{
    // Producer::start() + Consumer::start(); on_zmq_data callback fires with correct data.
    auto proc = SpawnWorker("hub_api.active_producer_consumer_callbacks", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, PeerCallbackOnConsumerJoin)
{
    // on_consumer_joined fires from peer_thread when consumer sends HELLO.
    auto proc = SpawnWorker("hub_api.peer_callback_on_consumer_join", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, NonTemplateFactory)
{
    // Non-template create/connect (no schema); SHM works; ZMQ works.
    auto proc = SpawnWorker("hub_api.non_template_factory", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ManagedProducerLifecycle)
{
    // Producer lifecycle: start()/stop()/close() idempotent; is_running() correct.
    auto proc = SpawnWorker("hub_api.managed_producer_lifecycle", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ConsumerShmSecretMismatch)
{
    // Wrong shm_shared_secret => consumer.shm() is nullptr; ZMQ transport still works.
    auto proc = SpawnWorker("hub_api.consumer_shm_secret_mismatch", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ConsumerByeTracked)
{
    // Consumer::close() sends BYE; on_consumer_left fires and connected_consumers empties.
    auto proc = SpawnWorker("hub_api.consumer_bye_tracked", {});
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

TEST_F(DatahubHubApiTest, ProducerConsumerCtrlMessaging)
{
    // Bidirectional ctrl: consumer->send_ctrl triggers on_consumer_message;
    // producer->send_ctrl triggers on_producer_message.
    auto proc = SpawnWorker("hub_api.producer_consumer_ctrl_messaging", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubApiTest, ConsumerDestructorBye)
{
    // Regression: Consumer destructor (no explicit stop) sends BYE;
    // on_consumer_left fires and connected_consumers empties.
    auto proc = SpawnWorker("hub_api.consumer_destructor_bye", {});
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

TEST_F(DatahubHubApiTest, ChecksumMismatchThroughForwarding)
{
    // End-to-end: Producer stamps checksum, Consumer verifies — two consecutive reads pass.
    auto proc = SpawnWorker("hub_api.checksum_mismatch_through_forwarding", {});
    ExpectWorkerOk(proc);
}
