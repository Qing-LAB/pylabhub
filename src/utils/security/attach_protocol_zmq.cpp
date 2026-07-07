/**
 * @file attach_protocol_zmq.cpp
 * @brief ZMQ ROUTER/DEALER binding of the AttachProtocol handshake
 *        (Phase 4c minimal — 2026-07-07).
 *
 * See `attach_protocol.hpp` for the public contract of
 * `ZmqAttachProtocolAcceptor` and `initiate_zmq_consumer_handshake`.
 *
 * These entries are thin composers over the transport-agnostic
 * `run_producer_handshake` / `run_consumer_handshake` helpers plus
 * the `ZmqAttachChannel` transport binding.  All Frame 1/2/3 crypto
 * flow, ABI constants, and error taxonomy live in
 * `attach_protocol.cpp` — this file adds ZERO new crypto code.
 *
 * Phase 4c-cont (deferred, tracked in AUTH_TODO.md) wires
 * `ZmqAttachProtocolAcceptor::run_handshake` into BrokerService's
 * REG_REQ handler as belt-and-braces on top of CURVE — that requires
 * reorganizing the broker's single-turn handler into a per-peer
 * state machine.  Out of scope here.
 */
#include "utils/security/attach_channel_zmq.hpp"
#include "utils/security/attach_protocol.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace pylabhub::utils::security
{

// ─────────────────────────────────────────────────────────────────
// ZmqAttachProtocolAcceptor
// ─────────────────────────────────────────────────────────────────

ZmqAttachProtocolAcceptor::ZmqAttachProtocolAcceptor(
    std::string             own_seckey_name,
    ObserverPubkeyAccessor  observer_pubkey_accessor)
    : own_seckey_name_(std::move(own_seckey_name)),
      observer_pubkey_accessor_(std::move(observer_pubkey_accessor))
{
    if (own_seckey_name_.empty())
    {
        throw std::invalid_argument(
            "ZmqAttachProtocolAcceptor: own_seckey_name must be non-empty");
    }
}

ProducerHandshakeResult
ZmqAttachProtocolAcceptor::run_handshake(
    zmq::socket_t             &router,
    const std::string         &peer_routing_id,
    std::chrono::milliseconds  timeout)
{
    if (peer_routing_id.empty())
    {
        throw std::invalid_argument(
            "ZmqAttachProtocolAcceptor: peer_routing_id must be non-empty "
            "(caller must capture the routing identity from a prior ROUTER "
            "recv before invoking the handshake)");
    }

    // Compute the shared handshake deadline once — the same discipline
    // as `AttachProtocolAcceptor::accept_one` uses on SHM.  Every
    // Frame's I/O respects this bound.
    const auto handshake_deadline =
        std::chrono::steady_clock::now() + timeout;

    // Wrap the ROUTER + routing_id in a channel.  Channel is
    // non-owning of the socket; the caller retains socket ownership.
    ZmqAttachChannel channel(router, peer_routing_id, "producer");

    // Delegate to the transport-agnostic producer handshake.
    return run_producer_handshake(channel, own_seckey_name_,
                                  observer_pubkey_accessor_,
                                  handshake_deadline);
}

// ─────────────────────────────────────────────────────────────────
// initiate_zmq_consumer_handshake
// ─────────────────────────────────────────────────────────────────

void
initiate_zmq_consumer_handshake(zmq::socket_t              &dealer,
                                const ConsumerAuthMaterial &self,
                                const std::string          &producer_pubkey_z85,
                                std::chrono::milliseconds   timeout,
                                bool                        require_mutual_auth)
{
    const auto handshake_deadline =
        std::chrono::steady_clock::now() + timeout;

    // DEALER side — empty routing_identity selects the single-part
    // multipart shape (`[json_body]`) in ZmqAttachChannel.
    ZmqAttachChannel channel(dealer, "" /*DEALER*/, "consumer");

    // Delegate to the transport-agnostic consumer handshake.  Frame 1
    // recv / Frame 2 send timeout bubbles up as AttachProtocolTimeout
    // (caller's decision to retry vs rethrow); Frame 3 timeout is
    // converted to std::runtime_error inside the helper.
    run_consumer_handshake(channel, self, producer_pubkey_z85,
                           handshake_deadline, require_mutual_auth);
}

} // namespace pylabhub::utils::security
