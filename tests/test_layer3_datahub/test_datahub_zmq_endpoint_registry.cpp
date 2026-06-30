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

// ── ENDPOINT_UPDATE error-path coverage (HEP-CORE-0021 §16.3 contract) ──
// The happy-path test above (EndpointUpdate_ReflectedInDiscovery) pins
// the sync REQ/REP success branch.  The two below pin the typed-error
// branches: BRC's send_endpoint_update returns a value-with-error (not
// nullopt) when the broker rejects, and the error_code is the typed
// one the HEP claims.  Mutation-sweep verified (flipping the broker
// to apply the rejected update would make these tests fail).

TEST_F(ZmqEndpointRegistryTest, EndpointUpdate_NonProducer_ReturnsError)
{
    auto w = SpawnWorker(
        "zmq_endpoint_registry.endpoint_update_non_producer_returns_error");
    ExpectWorkerOk(w);
}

TEST_F(ZmqEndpointRegistryTest, EndpointUpdate_PortZero_ReturnsError)
{
    auto w = SpawnWorker(
        "zmq_endpoint_registry.endpoint_update_port_zero_returns_error");
    ExpectWorkerOk(w);
}

// ── HEP-CORE-0007 §12.2.1 REQ-shape-conformance observability test ──────
// Drives every fire-and-forget REQ (HEARTBEAT, CHECKSUM_ERROR_REPORT,
// CHANNEL_BROADCAST, BAND_BROADCAST) and asserts the BRC's
// `unmatched_replies()` counter stays at zero — i.e. the broker did not
// silently emit any `_ACK` / `ERROR` that the BRC then dropped because
// no pending request was waiting.  This is the runtime fingerprint of
// the half-mix shape-contract violation HEP-0007 §12.2.1 prohibits.
// See `BrokerRequestComm::unmatched_replies()` for the counter.

TEST_F(ZmqEndpointRegistryTest, ReqShape_NoUnmatchedRepliesForFireAndForget)
{
    auto w = SpawnWorker(
        "zmq_endpoint_registry.req_shape_no_unmatched_replies_for_fire_and_forget");
    ExpectWorkerOk(w);
}

// ── RETIRED 2026-06-30 (#154 AUTH-6 C5 — handoff #307) ──────────────────
// Was: ReqShape_SyncReqTimesOutOnNoReply — HEP-CORE-0007 §12.2.1
// timeout-path conformance against a plain-TCP silent ROUTER stub.
// Why retired: HEP-CORE-0035 §2 strict-CURVE causes
// BrokerRequestComm::connect() to refuse any cfg without broker_pubkey +
// KeyStore role_identity.  The plain-TCP `StubBrcHandle` bypass the test
// depended on is no longer legal — connect() returns false before any
// REQ goes out.
// Coverage handoff: #307 reinstates this contract either via a
// CURVE-capable silent router fixture (option a) or an L2 test against
// BrokerRequestComm::do_request directly with a mock socket (option b).
