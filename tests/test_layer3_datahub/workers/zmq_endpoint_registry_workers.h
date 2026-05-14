#pragma once
/**
 * @file zmq_endpoint_registry_workers.h
 * @brief Workers for broker ZMQ endpoint registry tests
 *        (HEP-CORE-0021; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace zmq_endpoint_registry
{

int default_transport_is_shm();
int zmq_transport_round_trip();
int multiple_consumers_discover_same_endpoint();
int shm_and_zmq_coexist();
int endpoint_update_reflected_in_discovery();

} // namespace zmq_endpoint_registry
} // namespace pylabhub::tests::worker
