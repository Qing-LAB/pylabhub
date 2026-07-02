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

} // anon

struct BrokerWireClient::Impl
{
    zmq::socket_t dealer;
    std::string   broker_endpoint;

    Impl(zmq::context_t &ctx, const Config &cfg)
        : dealer(ctx, zmq::socket_type::dealer),
          broker_endpoint(cfg.broker_endpoint)
    {
        // Socket policy — match production BRC (linger=0, sndtimeo, no
        // reconnect throttle beyond default).  DEALER auto-assigns an
        // identity if we don't set one; leaving default is fine for
        // wire-shape tests because ROUTER strips the identity anyway.
        dealer.set(zmq::sockopt::linger, cfg.linger_ms);
        dealer.set(zmq::sockopt::sndtimeo, cfg.sndtimeo_ms);

        // CURVE — mandatory per HEP-CORE-0036 §7.4.  Order matches
        // BrokerRequestComm::connect (serverkey → publickey → secretkey).
        dealer.set(zmq::sockopt::curve_serverkey, cfg.broker_pubkey);
        dealer.set(zmq::sockopt::curve_publickey, cfg.client_pubkey);
        dealer.set(zmq::sockopt::curve_secretkey, cfg.client_seckey);

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
    // send_multipart throws on send failure (SNDTIMEO fires, socket in
    // bad state, etc.).  Tests treat that as a hard failure — the whole
    // point of the wire client is deterministic reproducibility.
    zmq::send_multipart(pImpl->dealer, frames);
}

std::optional<std::pair<std::string, nlohmann::json>>
BrokerWireClient::receive(std::chrono::milliseconds timeout)
{
    zmq::pollitem_t items[] = {
        {static_cast<void *>(pImpl->dealer), 0, ZMQ_POLLIN, 0},
    };
    const long timeout_ms = static_cast<long>(timeout.count());
    const int  n_ready    = zmq::poll(items, 1, std::chrono::milliseconds(timeout_ms));
    if (n_ready <= 0)
        return std::nullopt;

    std::vector<zmq::message_t> msgs;
    auto recv_n = zmq::recv_multipart(pImpl->dealer, std::back_inserter(msgs),
                                       zmq::recv_flags::dontwait);
    if (!recv_n.has_value() || *recv_n < 3)
        throw std::runtime_error(fmt::format(
            "BrokerWireClient::receive: expected 3-frame reply, got {}",
            recv_n.value_or(0)));

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

    // Poll loop — filter out any non-matching msg_types until either
    // the expected ACK arrives or the whole-call budget expires.
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
        if (next->first == expect_ack_type)
            return std::move(next->second);
        // Silently discard unrelated frames (typically NOTIFY).
    }
}

} // namespace pylabhub::tests::pattern4
