/**
 * @file attach_channel_zmq.cpp
 * @brief ZMQ implementation of `IAttachChannel` (Phase 4a).
 *
 * See `attach_channel_zmq.hpp` for the wire format (ROUTER vs DEALER
 * multipart shape), deadline discipline, and socket-ownership
 * contract.  This file wraps cppzmq's `send_multipart` / `recv_multipart`
 * with per-call `zmq_poll` timeouts so the caller's absolute deadline
 * bounds the full send-or-recv operation.
 */
#include "utils/security/attach_channel_zmq.hpp"
#include "utils/security/attach_protocol.hpp"  // AttachProtocolTimeout

#include <nlohmann/json.hpp>
#include "cppzmq/zmq.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace pylabhub::utils::security
{

namespace
{

// Deadline helper `attach_remaining_ms` is in `attach_channel.hpp`
// (Priority 3 / B2 consolidation, 2026-07-07).

// Poll for readability/writability with the remaining budget.
// Returns true if the socket is ready, false on deadline expiry.
// Throws on zmq_poll error (rare — EINTR retried internally).
bool
poll_for(zmq::socket_t &socket, short events,
         std::chrono::steady_clock::time_point deadline,
         const char *side, const char *what)
{
    while (true)
    {
        const auto rem = attach_remaining_ms(deadline);
        if (rem.count() == 0) return false;

        zmq::pollitem_t items[1] = {
            {socket.handle(), 0, events, 0}
        };
        const int rc = ::zmq_poll(items, 1, static_cast<long>(rem.count()));
        if (rc > 0) return true;
        if (rc == 0) return false;  // timeout — retry loop's deadline check
        const int e = zmq_errno();
        if (e == EINTR) continue;  // retry
        throw std::runtime_error(std::string{"AttachProtocol::"} + side +
                                  ": zmq_poll(" + what + ") failed: " +
                                  zmq_strerror(e));
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────
// ZmqAttachChannel::send_frame
// ─────────────────────────────────────────────────────────────────

void
ZmqAttachChannel::send_frame(const nlohmann::json                  &frame,
                              std::chrono::steady_clock::time_point  deadline)
{
    const std::string body = frame.dump();
    if (body.size() > IAttachChannel::kMaxAttachFrameBytes)
    {
        throw std::runtime_error(
            std::string{"AttachProtocol::"} + side_ +
            ": frame too large (" + std::to_string(body.size()) + " > " +
            std::to_string(IAttachChannel::kMaxAttachFrameBytes) + ")");
    }

    // Wait until the socket can accept a message (ZMQ_POLLOUT).
    if (!poll_for(socket_, ZMQ_POLLOUT, deadline, side_.c_str(), "POLLOUT"))
    {
        throw AttachProtocolTimeout(
            std::string{"AttachProtocol::"} + side_ +
            ": send deadline expired before ZMQ_POLLOUT ready");
    }

    // Build the multipart:
    //   ROUTER side (non-empty routing_identity_): [routing_id][body]
    //   DEALER side (empty routing_identity_):     [body]
    // ROUTER sockets REQUIRE the routing frame prefix — libzmq drops
    // messages whose first frame doesn't match a known peer identity
    // (with ZMQ_ROUTER_MANDATORY set; without it, they're silently
    // dropped, which we DO NOT rely on).  Caller must ensure the
    // socket was constructed with ZMQ_ROUTER_MANDATORY=1 if using
    // this channel with a ROUTER, so send failures raise loudly.
    const bool router_side = !routing_identity_.empty();

    try
    {
        if (router_side)
        {
            zmq::message_t rid_msg(routing_identity_.data(),
                                    routing_identity_.size());
            const auto rc1 = socket_.send(std::move(rid_msg),
                                           zmq::send_flags::sndmore |
                                           zmq::send_flags::dontwait);
            if (!rc1.has_value())
            {
                throw std::runtime_error(
                    std::string{"AttachProtocol::"} + side_ +
                    ": routing frame send failed (EAGAIN)");
            }
        }
        zmq::message_t body_msg(body.data(), body.size());
        const auto rc2 = socket_.send(std::move(body_msg),
                                       zmq::send_flags::dontwait);
        if (!rc2.has_value())
        {
            throw std::runtime_error(
                std::string{"AttachProtocol::"} + side_ +
                ": body frame send failed (EAGAIN — poll didn't guarantee "
                "capacity for full multipart)");
        }
    }
    catch (const zmq::error_t &e)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side_ +
                                  ": zmq send failed: " + e.what());
    }
}

// ─────────────────────────────────────────────────────────────────
// ZmqAttachChannel::recv_frame
// ─────────────────────────────────────────────────────────────────

nlohmann::json
ZmqAttachChannel::recv_frame(std::chrono::steady_clock::time_point deadline)
{
    // Wait for readable data.
    if (!poll_for(socket_, ZMQ_POLLIN, deadline, side_.c_str(), "POLLIN"))
    {
        throw AttachProtocolTimeout(
            std::string{"AttachProtocol::"} + side_ +
            ": recv deadline expired before ZMQ_POLLIN ready");
    }

    const bool router_side = !routing_identity_.empty();
    zmq::message_t incoming_rid;
    zmq::message_t incoming_body;

    try
    {
        if (router_side)
        {
            // Recv routing_id first, then body.
            auto rc1 = socket_.recv(incoming_rid, zmq::recv_flags::dontwait);
            if (!rc1.has_value())
            {
                throw std::runtime_error(
                    std::string{"AttachProtocol::"} + side_ +
                    ": routing frame recv failed (POLL said ready but "
                    "no message)");
            }
            if (!incoming_rid.more())
            {
                throw std::runtime_error(
                    std::string{"AttachProtocol::"} + side_ +
                    ": routing frame arrived without body (no more parts)");
            }
            // Verify routing_id matches this channel's expected peer.
            // A ROUTER socket shared across multiple peers could deliver
            // frames from a DIFFERENT peer than this channel handles;
            // reject and let the caller's dispatcher route to the right
            // channel.
            const std::string got(
                static_cast<const char *>(incoming_rid.data()),
                incoming_rid.size());
            if (got != routing_identity_)
            {
                throw std::runtime_error(
                    std::string{"AttachProtocol::"} + side_ +
                    ": ROUTER delivered frame from unexpected peer "
                    "(expected identity " +
                    std::to_string(routing_identity_.size()) +
                    " bytes, got " + std::to_string(got.size()) +
                    " bytes — cross-talk from another peer on shared "
                    "ROUTER)");
            }
        }
        auto rc2 = socket_.recv(incoming_body, zmq::recv_flags::dontwait);
        if (!rc2.has_value())
        {
            throw std::runtime_error(std::string{"AttachProtocol::"} + side_ +
                                     ": body frame recv failed");
        }
        if (incoming_body.more())
        {
            throw std::runtime_error(
                std::string{"AttachProtocol::"} + side_ +
                ": trailing frames after body (unexpected multipart)");
        }
    }
    catch (const zmq::error_t &e)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side_ +
                                  ": zmq recv failed: " + e.what());
    }

    // DoS cap check on the body.
    if (incoming_body.size() > IAttachChannel::kMaxAttachFrameBytes)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side_ +
                                  ": incoming frame size " +
                                  std::to_string(incoming_body.size()) +
                                  " exceeds DoS cap " +
                                  std::to_string(IAttachChannel::kMaxAttachFrameBytes));
    }

    const char *body_ptr  = static_cast<const char *>(incoming_body.data());
    const auto  body_size = incoming_body.size();
    try
    {
        return nlohmann::json::parse(body_ptr, body_ptr + body_size);
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side_ +
                                  ": JSON parse failed: " + e.what());
    }
}

} // namespace pylabhub::utils::security
