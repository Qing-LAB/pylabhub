#pragma once
/**
 * @file hub_federation_workers.h
 * @brief Workers for Hub Federation protocol tests (HEP-CORE-0022, Pattern 3).
 *
 * Three scenarios exercise the federation control plane:
 *   1. HUB_PEER_HELLO / HELLO_ACK handshake (on_hub_connected)
 *   2. HUB_TARGETED_MSG delivery (on_hub_message)
 *   3. HUB_PEER_BYE → on_hub_disconnected on the peer
 *
 * Each scenario spins up two in-process `BrokerService` instances, so
 * the body must run in a worker subprocess per
 * `docs/README/README_testing.md` § "Choosing a test pattern".
 */

namespace pylabhub::tests::worker
{
namespace hub_federation
{

int hello_handshake_fires_on_hub_connected();
int targeted_message_fires_on_hub_message();
int peer_bye_triggers_on_hub_disconnected();

} // namespace hub_federation
} // namespace pylabhub::tests::worker
