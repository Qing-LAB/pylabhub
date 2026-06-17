/**
 * @file shm_capability_channel.cpp
 * @brief Factory stubs for the cross-platform SHM capability transport.
 *
 * Carries no backend yet — factory functions throw `std::runtime_error`
 * naming task #249 which ships the Linux/FreeBSD `memfd_create`
 * backend.  The throws are the test contract: the L2 skeleton test
 * asserts they fire with the documented message, so accidentally
 * removing the throw without supplying a backend turns that test red.
 *
 * @see docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md §10 Phase 1.
 */
#include "utils/security/shm_capability_channel.hpp"

#include <stdexcept>

namespace pylabhub::utils::security
{

std::unique_ptr<IShmCapabilityProducer>
create_shm_capability_producer(size_t /*bytes*/)
{
    throw std::runtime_error(
        "HEP-CORE-0041 capability transport: no ShmCapabilityProducer "
        "backend registered yet (pending task #249 — memfd_create "
        "backend for Linux/FreeBSD).");
}

std::unique_ptr<IShmCapabilityConsumer>
attach_shm_capability_consumer(const std::string & /*endpoint*/,
                               std::chrono::milliseconds /*timeout*/)
{
    throw std::runtime_error(
        "HEP-CORE-0041 capability transport: no ShmCapabilityConsumer "
        "backend registered yet (pending task #249 — memfd_create "
        "backend for Linux/FreeBSD).");
}

} // namespace pylabhub::utils::security
