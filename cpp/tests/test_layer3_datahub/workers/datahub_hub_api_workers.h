#pragma once
// tests/test_layer3_datahub/workers/datahub_hub_api_workers.h
//
// Hub Producer/Consumer unified API tests (Phase: hub_api).

namespace pylabhub::tests::worker::hub_api
{

/** Producer::create(PubSub, no shm): is_valid, channel_name, close. */
int producer_create_pubsub(int argc, char **argv);

/** Producer::create(has_shm): synced_write(job) executes sync slot; push async. */
int producer_create_with_shm(int argc, char **argv);

/** Producer + Consumer via unified API; ZMQ send/recv end-to-end. */
int consumer_connect_e2e(int argc, char **argv);

/** After Consumer::connect(), producer.connected_consumers() contains identity. */
int consumer_hello_tracked(int argc, char **argv);

/** Producer::start() + Consumer::start(); on_zmq_data callback fires. */
int active_producer_consumer_callbacks(int argc, char **argv);

/** Producer::on_consumer_joined fires from peer_thread when consumer connects. */
int peer_callback_on_consumer_join(int argc, char **argv);

/** Non-template create/connect (no schema); shm works; ZMQ works. */
int non_template_factory(int argc, char **argv);

/** ManagedProducer + LifecycleGuard: get() valid post-init; close() on teardown. */
int managed_producer_lifecycle(int argc, char **argv);

/** Wrong shm_shared_secret => consumer.shm() is nullptr; ZMQ still works. */
int consumer_shm_secret_mismatch(int argc, char **argv);

/** Consumer::close() sends BYE; connected_consumers drops to 0; on_consumer_left fires. */
int consumer_bye_tracked(int argc, char **argv);

/** Producer push → consumer set_read_handler fires with correct data (active SHM path). */
int consumer_shm_read_e2e(int argc, char **argv);

/** Producer synced_write (sync) → Consumer pull (sync): data round-trip verified. */
int consumer_read_shm_sync(int argc, char **argv);

/** start()/stop()/close() called twice each is safe (idempotency invariant). */
int producer_consumer_idempotency(int argc, char **argv);

/** consumer->send_ctrl → on_consumer_message fires; producer->send_ctrl → on_producer_message fires. */
int producer_consumer_ctrl_messaging(int argc, char **argv);

/** Consumer destructor (no explicit stop) still sends BYE; connected_consumers empties. */
int consumer_destructor_bye(int argc, char **argv);

} // namespace pylabhub::tests::worker::hub_api
