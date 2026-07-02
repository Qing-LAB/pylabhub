/**
 * @file broker_wire_client.cpp
 * @brief BrokerWireClient impl — raw DEALER wire client for L3 broker tests.
 */
#include "broker_wire_client.h"

#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include <cppzmq/zmq_addon.hpp>
#include <fmt/format.h>

namespace pylabhub::tests::pattern4 {

namespace {

// Matches BrokerService + BrokerRequestComm frame-type control byte.
constexpr char kFrameTypeControl = 'C';

// Broker's canonical error msg_type.  Match BrokerService::send_reply's
// error envelope — a caller that expected some ACK but got ERROR wants
// the error body returned rather than silently discarded.
constexpr char kErrorMsgType[] = "ERROR";

} // anon

struct BrokerWireClient::Impl
{
    zmq::socket_t dealer;
    std::string   broker_endpoint;

    Impl(zmq::context_t &ctx, const Config &cfg)
        : dealer(ctx, zmq::socket_type::dealer),
          broker_endpoint(cfg.broker_endpoint)
    {
        // Socket policy — match production BRC.
        dealer.set(zmq::sockopt::linger, cfg.linger_ms);
        dealer.set(zmq::sockopt::sndtimeo, cfg.sndtimeo_ms);
        // Reconnect disabled per HEP-CORE-0023 §2.5.3 "disconnection is
        // terminal" — matches BRC's `apply_socket_policy`.  Without
        // this, libzmq's default 100 ms auto-reconnect would silently
        // reconnect to a respawned broker on the same endpoint,
        // masking crash-restart scenarios that tests need to observe.
        dealer.set(zmq::sockopt::reconnect_ivl, -1);

        // CURVE — mandatory per HEP-CORE-0036 §7.4.  Order matches
        // BrokerRequestComm::connect (serverkey → publickey → secretkey).
        dealer.set(zmq::sockopt::curve_serverkey, cfg.broker_pubkey);
        dealer.set(zmq::sockopt::curve_publickey, cfg.client_pubkey);
        dealer.set(zmq::sockopt::curve_secretkey, cfg.client_seckey);

        // TCP connect is asynchronous; a bad endpoint / server-key
        // does NOT throw here — it surfaces later as a receive-side
        // timeout.  Callers gate on "handshake succeeded" by sending
        // a lightweight REQ and waiting for the reply.
        dealer.connect(cfg.broker_endpoint);
    }
};

BrokerWireClient::BrokerWireClient(zmq::context_t &ctx, const Config &cfg)
    : pImpl(std::make_unique<Impl>(ctx, cfg))
{
}

BrokerWireClient::~BrokerWireClient() = default;

void BrokerWireClient::send(std::string_view      msg_type,
                             const nlohmann::json &body)
{
    const std::string body_str = body.dump();
    std::vector<zmq::const_buffer> frames = {
        zmq::buffer(&kFrameTypeControl, 1),
        zmq::buffer(msg_type),
        zmq::buffer(body_str),
    };
    // `send_multipart` throws `zmq::error_t` on SNDTIMEO / socket-in-
    // bad-state — matches BRC.  Contract on the header: this is by
    // design and callers under stress paths should catch or drain
    // between bursts.
    zmq::send_multipart(pImpl->dealer, frames);
}

std::optional<std::pair<std::string, nlohmann::json>>
BrokerWireClient::receive(std::chrono::milliseconds timeout)
{
    // Guard negative / zero timeouts — a caller-supplied `remaining`
    // that has already elapsed (or a raw negative literal) would
    // otherwise become `zmq_poll(timeout=-N)` which blocks forever
    // per libzmq semantics.  Treat as "one non-blocking check".
    if (timeout < std::chrono::milliseconds{0})
        timeout = std::chrono::milliseconds{0};

    zmq::pollitem_t items[] = {
        {static_cast<void *>(pImpl->dealer), 0, ZMQ_POLLIN, 0},
    };
    const int n_ready = zmq::poll(items, 1, timeout);
    if (n_ready <= 0)
        return std::nullopt;

    std::vector<zmq::message_t> msgs;
    auto recv_n = zmq::recv_multipart(pImpl->dealer, std::back_inserter(msgs),
                                       zmq::recv_flags::dontwait);
    if (!recv_n.has_value())
    {
        // Poll said readable, but recv_multipart yielded no message —
        // spurious wake (rare but possible with TCP-DEALER contention).
        // Distinguish from short/over-frame count so debugging isn't
        // misled by "got 0" wording.
        throw std::runtime_error(
            "BrokerWireClient::receive: poll reported readable but "
            "recv_multipart returned no message (spurious wake?)");
    }
    if (*recv_n != 3)
        throw std::runtime_error(fmt::format(
            "BrokerWireClient::receive: 3-frame wire violation — got "
            "{} frames (expected [C, msg_type, body])",
            *recv_n));

    // Frame 0: control byte 'C' — sanity-check.
    if (msgs[0].size() != 1 ||
        *static_cast<const char *>(msgs[0].data()) != kFrameTypeControl)
    {
        throw std::runtime_error(
            "BrokerWireClient::receive: frame 0 is not 'C' control byte");
    }

    std::string msg_type = msgs[1].to_string();
    nlohmann::json body;
    try
    {
        body = nlohmann::json::parse(msgs[2].to_string());
    }
    catch (const nlohmann::json::exception &e)
    {
        throw std::runtime_error(fmt::format(
            "BrokerWireClient::receive: body is not valid JSON: {}", e.what()));
    }
    return std::make_pair(std::move(msg_type), std::move(body));
}

std::optional<nlohmann::json>
BrokerWireClient::request(std::string_view      req_type,
                           const nlohmann::json &body,
                           std::string_view      expect_ack_type,
                           std::chrono::milliseconds timeout)
{
    const auto start = std::chrono::steady_clock::now();
    send(req_type, body);

    // Poll loop — return on expected ACK OR on any ERROR (broker
    // rejected the request; return the ERROR body so the caller sees
    // the diagnostic reason instead of burning the full budget on a
    // reply that will never match).  Silently drop unrelated frames
    // (typically NOTIFY).
    while (true)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                timeout - elapsed);
        if (remaining <= std::chrono::milliseconds{0})
            return std::nullopt;

        auto next = receive(remaining);
        if (!next.has_value())
            return std::nullopt;
        if (next->first == expect_ack_type || next->first == kErrorMsgType)
            return std::move(next->second);
        // Silently discard unrelated frames (typically NOTIFY).
    }
}

} // namespace pylabhub::tests::pattern4
