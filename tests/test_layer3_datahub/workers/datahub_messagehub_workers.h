#pragma once

namespace pylabhub::tests::worker::messagehub
{
/** Lifecycle: hub module initialized => lifecycle_initialized() true. */
int lifecycle_initialized_follows_state();
/** When not connected, discover_producer returns std::nullopt (replaces send_message test). */
int send_message_when_not_connected_returns_nullopt();
/** When not connected, discover_producer returns std::nullopt (replaces receive_message test). */
int receive_message_when_not_connected_returns_nullopt();
/** When not connected, register_producer (void/fire-and-forget) does not throw or crash. */
int register_producer_when_not_connected_returns_false();
/** When not connected, discover_producer returns std::nullopt. */
int discover_producer_when_not_connected_returns_nullopt();
/** disconnect() when not connected is idempotent (no crash). */
int disconnect_when_not_connected_idempotent();
/** Phase C.1: In-process broker; manual register_producer, discover_producer, create/find, one write/read. */
int with_broker_happy_path();
/** query_channel_schema when not connected returns nullopt. */
int query_channel_schema_not_connected();
/** create_channel when not connected returns false (nullopt). */
int create_channel_not_connected();
/** connect_channel when not connected returns nullopt. */
int connect_channel_not_connected();
/** suppress/enqueue heartbeat when worker not running is no-op. */
int heartbeat_noop_not_running();
/** on_channel_closing global callback registration does not crash. */
int on_channel_closing_global_register();
/** on_channel_closing per-channel register then deregister (nullptr). */
int on_channel_closing_register_deregister();
/** on_consumer_died callback registration does not crash. */
int on_consumer_died_register();
} // namespace pylabhub::tests::worker::messagehub
