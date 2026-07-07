#pragma once
/**
 * @file hub_host_integration_workers.h
 * @brief Workers for HubHost ↔ BrokerService L3 integration tests (Pattern 3).
 *
 * Two scenarios verify the HubHost-spawned broker actually serves
 * protocol traffic — basic reachability + REG_REQ round-trip.  Pattern 3
 * isolation: each scenario constructs a `HubHost` (which spawns a
 * `BrokerService` thread) and runs a `BrokerRequestComm` poll loop
 * against it, transitively touching Logger / FileLock / JsonConfig /
 * SecureSubsystem / ZMQContext.  Cross-process hub-death observability is
 * tracked as L4 task #296 (HEP-CORE-0023 §2.5.3).
 */

namespace pylabhub::tests::worker
{
namespace hub_host_integration
{

int hubhost_brokerreachable_afterstartup();
int hubhost_regreq_roundtripsviaspawnedbroker();

} // namespace hub_host_integration
} // namespace pylabhub::tests::worker
