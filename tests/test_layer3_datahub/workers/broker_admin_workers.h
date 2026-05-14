#pragma once
/**
 * @file broker_admin_workers.h
 * @brief Workers for BrokerService admin API tests
 *        (list_channels_json_str / query_channel_snapshot /
 *        request_close_channel; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace broker_admin
{

int list_channels_empty();
int list_channels_one_channel();
int list_channels_field_presence();
int snapshot_empty();
int snapshot_one_channel();
int snapshot_after_consumer();
int close_channel_existing();
int close_channel_non_existent();

} // namespace broker_admin
} // namespace pylabhub::tests::worker
