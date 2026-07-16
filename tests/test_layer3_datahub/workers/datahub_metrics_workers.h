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

// Storage / wire tests MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_metrics.cpp (task #52 Round 3 — broker
// HeartbeatMetricsStored trace): heartbeat_metrics_stored_by_broker,
// consumer_heartbeat_metrics_stored_by_broker,
// heartbeat_no_metrics_backward_compat, metrics_update_overwrite_on_heartbeat,
// producer_pid_in_query_result, fan_in_two_producers_metrics_do_not_overwrite,
// old_metrics_report_req_gets_unknown_msg_type.
//
// The query-engine tests below stay: they exercise the query_metrics()
// output/filter semantics (a query API, no storage trace to read).
// Disposition (L2 against the query engine, or a METRICS_REQ wire query)
// tracked in docs/todo/TESTING_TODO.md.
int query_metrics_unknown_channel_returns_empty();
int query_metrics_all_channels();
int query_engine_empty_filter_all_categories_present();
int query_engine_category_filter_only_broker();
int query_engine_channel_identity_filter();
int query_engine_roles_carry_collected_at();
int query_engine_channels_have_producer_and_consumer_metrics();
int query_engine_filter_echo();
int multi_presence_end_to_end_no_cross_attribution();
int all_channels_includes_channels_without_metrics();

} // namespace datahub_metrics
} // namespace pylabhub::tests::worker
