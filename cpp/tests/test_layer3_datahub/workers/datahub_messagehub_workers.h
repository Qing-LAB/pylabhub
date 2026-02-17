#pragma once

namespace pylabhub::tests::worker::messagehub
{
/** Lifecycle: hub module initialized => lifecycle_initialized() true. */
int lifecycle_initialized_follows_state();
/** When not connected, send_message returns std::nullopt. */
int send_message_when_not_connected_returns_nullopt();
/** When not connected, receive_message returns std::nullopt. */
int receive_message_when_not_connected_returns_nullopt();
/** When not connected, register_producer returns false. */
int register_producer_when_not_connected_returns_false();
/** When not connected, discover_producer returns std::nullopt. */
int discover_producer_when_not_connected_returns_nullopt();
/** disconnect() when not connected is idempotent (no crash). */
int disconnect_when_not_connected_idempotent();
/** Phase C.1: In-process broker; register_producer, discover_producer, create/find, one write/read. */
int with_broker_happy_path();
} // namespace pylabhub::tests::worker::messagehub
