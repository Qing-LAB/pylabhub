/**
 * @file test_datahub_zmq_endpoint_registry.cpp
 * @brief Pattern 3 driver — broker ZMQ endpoint registry tests
 *        (HEP-CORE-0021 — broker as directory for ZMQ peer endpoints).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/zmq_endpoint_registry_workers.cpp`.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class ZmqEndpointRegistryTest : public IsolatedProcessTest
{
};

TEST_F(ZmqEndpointRegistryTest, DefaultTransport_IsShm)
{
    auto w = SpawnWorker("zmq_endpoint_registry.default_transport_is_shm");
    ExpectWorkerOk(w);
}

TEST_F(ZmqEndpointRegistryTest, ZmqTransport_RoundTrip)
{
    auto w = SpawnWorker("zmq_endpoint_registry.zmq_transport_round_trip");
    ExpectWorkerOk(w);
}

TEST_F(ZmqEndpointRegistryTest, MultipleConsumers_DiscoverSameEndpoint)
{
    auto w = SpawnWorker(
        "zmq_endpoint_registry.multiple_consumers_discover_same_endpoint");
    ExpectWorkerOk(w);
}

TEST_F(ZmqEndpointRegistryTest, ShmAndZmq_Coexist)
{
    auto w = SpawnWorker("zmq_endpoint_registry.shm_and_zmq_coexist");
    ExpectWorkerOk(w);
}

TEST_F(ZmqEndpointRegistryTest, EndpointUpdate_ReflectedInDiscovery)
{
    auto w = SpawnWorker(
        "zmq_endpoint_registry.endpoint_update_reflected_in_discovery");
    ExpectWorkerOk(w);
}
