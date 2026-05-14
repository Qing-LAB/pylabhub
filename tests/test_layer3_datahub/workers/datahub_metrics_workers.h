#pragma once
/**
 * @file datahub_metrics_workers.h
 * @brief Workers for broker metrics-plane tests
 *        (HEP-CORE-0019; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace datahub_metrics
{

int heartbeat_metrics_stored_by_broker();
int consumer_heartbeat_metrics_stored_by_broker();
int query_metrics_unknown_channel_returns_empty();
int query_metrics_all_channels();
int heartbeat_no_metrics_backward_compat();
int metrics_update_overwrite_on_heartbeat();
int producer_pid_in_query_result();
int query_engine_empty_filter_all_categories_present();
int query_engine_category_filter_only_broker();
int query_engine_channel_identity_filter();
int query_engine_roles_carry_collected_at();
int query_engine_channels_have_producer_and_consumer_metrics();
int fan_in_two_producers_metrics_do_not_overwrite();
int query_engine_filter_echo();
int old_metrics_report_req_gets_unknown_msg_type();
int multi_presence_end_to_end_no_cross_attribution();
int all_channels_includes_channels_without_metrics();

} // namespace datahub_metrics
} // namespace pylabhub::tests::worker
