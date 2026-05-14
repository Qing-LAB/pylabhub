#pragma once
/**
 * @file hub_host_integration_workers.h
 * @brief Workers for HubHost ↔ BrokerService L3 integration tests (Pattern 3).
 *
 * Three scenarios verify the HubHost-spawned broker actually serves
 * protocol traffic — REG_REQ round-trip, post-shutdown probe, basic
 * reachability.  Pattern 3 isolation: each scenario constructs a
 * `HubHost` (which spawns a `BrokerService` thread) and runs a
 * `BrokerRequestComm` poll loop against it, transitively touching
 * Logger / FileLock / JsonConfig / CryptoUtils / ZMQContext.
 */

namespace pylabhub::tests::worker
{
namespace hub_host_integration
{

int hubhost_brokerreachable_afterstartup();
int hubhost_regreq_roundtripsviaspawnedbroker();
int hubhost_shutdown_breaksclientconnection();

} // namespace hub_host_integration
} // namespace pylabhub::tests::worker
