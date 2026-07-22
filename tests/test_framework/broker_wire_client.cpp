/**
 * @file broker_wire_client.cpp
 * @brief BrokerWireClient impl — raw DEALER wire client for L3 broker tests.
 */
#include "broker_wire_client.h"

#include "utils/security/secure_subsystem.hpp"
#include "utils/wire_adapter.hpp"
#include "utils/wire_envelope.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include <cppzmq/zmq_addon.hpp>
#include <fmt/format.h>

namespace pylabhub::tests::pattern4
{

namespace
{

// Broker's canonical error msg_type.  Match BrokerService::send_reply's
// error envelope — a caller that expected some ACK but got ERROR wants
// the error body returned rather than silently discarded.
constexpr char kErrorMsgType[] = "ERROR";

/// Generate a fresh 16-byte random hex correlation_id (32 hex chars).
/// Cryptographically random per I-CORRELATION-STABLE + I-REPLAY-BOUND.
std::string make_random_hex16()
{
    std::array<std::uint8_t, 16> raw{};
    namespace sec = pylabhub::utils::security;
    sec::secure().random_bytes(raw);
    char hex[33] = {};
    sec::secure().bin2hex(hex, sizeof(hex), raw.data(), raw.size());
    return std::string(hex, 32);
}

std::uint64_t system_wall_now_ms()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace

struct BrokerWireClient::Impl
{
    zmq::socket_t dealer;
    std::string broker_endpoint;
    std::string client_role_uid;

    Impl(zmq::context_t &ctx, const Config &cfg)
        : dealer(ctx, zmq::socket_type::dealer), broker_endpoint(cfg.broker_endpoint),
          client_role_uid(cfg.client_role_uid)
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

        // HEP-CORE-0046 I-DEALER-IDENTITY — routing_id = role_uid, set
        // before connect so broker's envelope parse sees a stable
        // Frame 0.
        if (client_role_uid.empty())
            throw std::runtime_error("BrokerWireClient: Config::client_role_uid is empty; "
                                     "I-DEALER-IDENTITY requires non-empty routing_id.");
        dealer.set(zmq::sockopt::routing_id, client_role_uid);

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

void BrokerWireClient::send(std::string_view msg_type, const nlohmann::json &body)
{
    // HEP-CORE-0046 §14 envelope encoder.  If the caller pre-set
    // `body["correlation_id"]` (non-empty string), use it verbatim so
    // conformance tests can pin the value the broker must echo.  Else
    // generate a fresh random 32-hex id per I-CORRELATION-STABLE.
    std::string corr_id;
    if (body.is_object())
    {
        auto it = body.find("correlation_id");
        if (it != body.end() && it->is_string())
            corr_id = it->get<std::string>();
    }
    if (corr_id.empty())
        corr_id = make_random_hex16();
    ::pylabhub::wire::adapter::EncodeContext ctx;
    ctx.dealer_role_uid = pImpl->client_role_uid;
    ctx.correlation_id = corr_id;

    std::string nonce_holder;
    if (::pylabhub::wire::adapter::msg_type_carries_security_triple(msg_type))
    {
        nonce_holder = make_random_hex16();
        ctx.client_nonce = nonce_holder;
        ctx.client_wall_ts = system_wall_now_ms();
    }
    // `send_multipart` throws `zmq::error_t` on SNDTIMEO / socket-in-
    // bad-state — matches BRC.  Contract on the header: this is by
    // design and callers under stress paths should catch or drain
    // between bursts.
    zmq::multipart_t wire = ::pylabhub::wire::adapter::encode_dealer_send(msg_type, ctx, body);
    wire.send(pImpl->dealer);
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

    zmq::multipart_t raw;
    if (!raw.recv(pImpl->dealer, ZMQ_DONTWAIT))
    {
        throw std::runtime_error("BrokerWireClient::receive: poll reported readable but "
                                 "multipart recv returned no message (spurious wake?)");
    }

    ::pylabhub::wire::ParseError err = {};
    auto env_opt = ::pylabhub::wire::WireEnvelope::parse_dealer_recv(std::move(raw),
                                                                     pImpl->client_role_uid, &err);
    if (!env_opt.has_value())
    {
        throw std::runtime_error(fmt::format("BrokerWireClient::receive: envelope parse failed "
                                             "(ParseError={})",
                                             static_cast<int>(err)));
    }
    ::pylabhub::wire::WireEnvelope env = std::move(*env_opt);
    std::string msg_type = std::string(env.msg_type());
    std::string correlation_id = std::string(env.correlation_id());
    nlohmann::json body = env.body();
    // Inject correlation_id into body so tests that inspect
    // body["correlation_id"] see the envelope value (matches BRC's
    // legacy-handler compat pattern).
    if (!correlation_id.empty())
        body["correlation_id"] = correlation_id;
    return std::make_pair(std::move(msg_type), std::move(body));
}

std::optional<nlohmann::json> BrokerWireClient::request(std::string_view req_type,
                                                        const nlohmann::json &body,
                                                        std::string_view expect_ack_type,
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
            std::chrono::duration_cast<std::chrono::milliseconds>(timeout - elapsed);
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
