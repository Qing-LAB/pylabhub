#pragma once
// tests/test_layer3_datahub/workers/datahub_channel_workers.h
//
// Phase 6 — ChannelHandle tests: create_channel / connect_channel send/recv.

namespace pylabhub::tests::worker::channel
{

/** create_channel returns std::nullopt when Messenger is not connected. */
int create_not_connected(int argc, char** argv);

/** connect_channel returns std::nullopt when channel does not exist (timeout). */
int connect_not_found(int argc, char** argv);

/** Pipeline create_channel + connect_channel + producer send + consumer recv. */
int pipeline_exchange(int argc, char** argv);

/** PubSub create_channel + connect_channel + producer send (retry) + consumer recv. */
int pubsub_exchange(int argc, char** argv);

/** ChannelHandle introspection: channel_name, pattern, is_valid. */
int channel_introspection(int argc, char** argv);

} // namespace pylabhub::tests::worker::channel
