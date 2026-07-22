#pragma once
/**
 * @file admin_wire_client.h
 * @brief DEALER operator-console test client for the typed admin protocol
 *        (HEP-CORE-0033 §11).
 *
 * Mirrors the production console: a persistent CURVE-client `DEALER` that
 * establishes a session (`ADMIN_HELLO_REQ` → sealed `session_id`), then sends
 * typed commands carrying that session id and receives typed acks.  Speaks the
 * same `WireEnvelope` the role plane uses (`build_dealer_send` /
 * `parse_dealer_recv`), so it exercises the real receive→typed-body→handler
 * path with zero down-conversion.
 *
 * Header-only: it only *calls* the utils-linked `WireEnvelope`; any test that
 * already links `pylabhub::utils` + `pylabhub::test_framework` can use it.
 */

#include "utils/wire_bodies.hpp"
#include "utils/wire_envelope.hpp"

#include <cppzmq/zmq.hpp>
#include <cppzmq/zmq_addon.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::tests
{

class AdminWireClient
{
  public:
    struct Config
    {
        std::string admin_endpoint; ///< tcp://… the admin ROUTER bind.
        std::string server_pubkey;  ///< hub broker pubkey (z85) — curve_serverkey.
        std::string client_pubkey;  ///< ephemeral client CURVE pub (z85).
        std::string client_seckey;  ///< ephemeral client CURVE sec (z85).
        std::string routing_id;     ///< the DEALER's own console routing id.
        int rcvtimeo_ms = 2000;
    };

    AdminWireClient(zmq::context_t &ctx, const Config &cfg)
        : dealer_(ctx, zmq::socket_type::dealer), routing_id_(cfg.routing_id)
    {
        dealer_.set(zmq::sockopt::linger, 0);
        dealer_.set(zmq::sockopt::routing_id, cfg.routing_id);
        dealer_.set(zmq::sockopt::rcvtimeo, cfg.rcvtimeo_ms);
        dealer_.set(zmq::sockopt::sndtimeo, cfg.rcvtimeo_ms);
        dealer_.set(zmq::sockopt::curve_publickey, cfg.client_pubkey);
        dealer_.set(zmq::sockopt::curve_secretkey, cfg.client_seckey);
        dealer_.set(zmq::sockopt::curve_serverkey, cfg.server_pubkey);
        dealer_.connect(cfg.admin_endpoint);
    }

    /// A parsed typed reply.
    struct Reply
    {
        std::string msg_type;
        nlohmann::json body;
        [[nodiscard]] bool is_error() const { return msg_type == std::string(wire::kAdminError); }
    };

    /// Send a typed request and receive the typed reply.  Returns nullopt on
    /// timeout or a malformed reply.
    std::optional<Reply> request(std::string_view msg_type, nlohmann::json body)
    {
        const std::string corr = "c" + std::to_string(++corr_ctr_);
        auto out =
            wire::WireEnvelope::build_dealer_send(routing_id_, msg_type, corr, std::move(body));
        out.send(dealer_);

        zmq::multipart_t in;
        if (!in.recv(dealer_))
            return std::nullopt; // rcvtimeo elapsed
        wire::ParseError perr{};
        auto env = wire::WireEnvelope::parse_dealer_recv(std::move(in), routing_id_, &perr);
        if (!env)
            return std::nullopt;
        return Reply{std::string(env->msg_type()), env->body()};
    }

    /// Establish the session: ADMIN_HELLO_REQ{token,label} → sealed session_id.
    /// Returns true and stores the session id on success.
    bool establish(std::string_view token, std::string_view label)
    {
        auto r = request(wire::kAdminHelloReq, nlohmann::json{{"token", std::string(token)},
                                                              {"label", std::string(label)}});
        if (!r || r->is_error() || r->msg_type != std::string(wire::kAdminHelloAck))
            return false;
        session_id_ = r->body.value("session_id", std::string{});
        return !session_id_.empty();
    }

    /// Send a command with the session id + a fresh replay triple
    /// (client_nonce, client_wall_ts) injected — the in-session replay guard
    /// (HEP-CORE-0033 §11.0.5).  Each call uses a unique nonce; to test replay
    /// rejection, build the body yourself and call request() twice.
    std::optional<Reply> command(std::string_view msg_type,
                                 nlohmann::json body = nlohmann::json::object())
    {
        body["session_id"] = session_id_;
        body["client_nonce"] = "n" + std::to_string(++nonce_ctr_);
        body["client_wall_ts"] = now_ms();
        return request(msg_type, std::move(body));
    }

    /// Current wall clock in ms — for building explicit command bodies.
    static std::uint64_t now_ms()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }

    [[nodiscard]] const std::string &session_id() const { return session_id_; }

  private:
    zmq::socket_t dealer_;
    std::string routing_id_;
    std::string session_id_;
    int corr_ctr_ = 0;
    std::uint64_t nonce_ctr_ = 0;
};

} // namespace pylabhub::tests
