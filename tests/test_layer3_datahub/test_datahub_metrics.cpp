/**
 * @file test_datahub_metrics.cpp
 * @brief Pattern 3 driver — broker metrics-plane tests
 *        (HEP-CORE-0019 + HEP-CORE-0033 §10.3).
 *
 * Migrated 2026-05-14 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/datahub_metrics_workers.cpp`.
 *
 * The worker file's first commit preserves the original
 * `LocalBrokerHandle` (mock-host) shape verbatim; the immediately-
 * following commit refactors to real `HubHost` per the no-mocks
 * principle — same two-commit sequence as `zmq_endpoint_registry`.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class MetricsPlaneTest : public IsolatedProcessTest
{
};

// Storage / wire tests MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_metrics.cpp (task #52 Round 3): HeartbeatMetrics /
// ConsumerHeartbeatMetrics / HeartbeatNoMetrics / MetricsUpdate_Overwrite /
// ProducerPID / FanIn_TwoProducers / OldMetricsReportReq.

TEST_F(MetricsPlaneTest, QueryMetrics_UnknownChannel_ReturnsEmpty)
{
    auto w = SpawnWorker(
        "datahub_metrics.query_metrics_unknown_channel_returns_empty");
    ExpectWorkerOk(w);
}

TEST_F(MetricsPlaneTest, QueryMetrics_AllChannels)
{
    auto w = SpawnWorker("datahub_metrics.query_metrics_all_channels");
    ExpectWorkerOk(w);
}

TEST_F(MetricsPlaneTest, QueryEngine_EmptyFilter_AllCategoriesPresent)
{
    auto w = SpawnWorker(
        "datahub_metrics.query_engine_empty_filter_all_categories_present");
    ExpectWorkerOk(w);
}

TEST_F(MetricsPlaneTest, QueryEngine_CategoryFilter_OnlyBroker)
{
    auto w = SpawnWorker(
        "datahub_metrics.query_engine_category_filter_only_broker");
    ExpectWorkerOk(w);
}

TEST_F(MetricsPlaneTest, QueryEngine_ChannelIdentityFilter)
{
    auto w = SpawnWorker(
        "datahub_metrics.query_engine_channel_identity_filter");
    ExpectWorkerOk(w);
}

TEST_F(MetricsPlaneTest, QueryEngine_RolesCarryCollectedAt)
{
    auto w = SpawnWorker(
        "datahub_metrics.query_engine_roles_carry_collected_at");
    ExpectWorkerOk(w);
}

TEST_F(MetricsPlaneTest, QueryEngine_ChannelsHaveProducerAndConsumerMetrics)
{
    auto w = SpawnWorker(
        "datahub_metrics.query_engine_channels_have_producer_and_consumer_"
        "metrics");
    ExpectWorkerOk(w);
}

TEST_F(MetricsPlaneTest, QueryEngine_FilterEcho)
{
    auto w = SpawnWorker("datahub_metrics.query_engine_filter_echo");
    ExpectWorkerOk(w);
}

TEST_F(MetricsPlaneTest, MultiPresence_EndToEnd_NoCrossAttribution)
{
    auto w = SpawnWorker(
        "datahub_metrics.multi_presence_end_to_end_no_cross_attribution");
    ExpectWorkerOk(w);
}

TEST_F(MetricsPlaneTest, AllChannels_IncludesChannelsWithoutMetrics)
{
    auto w = SpawnWorker(
        "datahub_metrics.all_channels_includes_channels_without_metrics");
    ExpectWorkerOk(w);
}
