#pragma once
/**
 * @file attach_channel_zmq.hpp
 * @brief ZMQ ROUTER binding of `IAttachChannel` — the transport
 *        adapter for AttachProtocol handshakes over ZMQ (Phase 4).
 *
 * @author  pyLabHub team
 * @date    2026-07-07 (Phase 4a — parallel to `attach_channel_shm.hpp`)
 * @copyright  MIT
 * @see  attach_channel.hpp  (interface + design rationale)
 * @see  attach_channel_shm.hpp  (SHM/AF_UNIX counterpart)
 *
 * # Wire format
 *
 * Each JSON frame is transmitted as a ZMQ multipart message.  The
 * multipart shape depends on the socket type the caller passes in:
 *
 * - **ROUTER side (broker/producer accepting peers):**
 *   ```
 *   [ routing_id ]  [ json_body ]
 *   ```
 *   The routing identity is the ZMQ ROUTER auto-prefix that
 *   identifies the peer.  The channel stores it at construction and
 *   auto-prepends on send / auto-verifies on recv (rejects
 *   cross-talk from other peers on the same ROUTER).
 *
 * - **DEALER side (role connecting to broker/producer):**
 *   ```
 *   [ json_body ]
 *   ```
 *   No routing identity — DEALER's connection is 1:1 with the peer.
 *   Construct with an empty `routing_identity` to select this shape.
 *
 * The transport-layer 4 KiB DoS cap (`kMaxAttachFrameBytes` in
 * `attach_channel.hpp`) applies uniformly.
 *
 * # Deadline discipline
 *
 * Both `send_frame` and `recv_frame` take an absolute
 * `steady_clock::time_point`.  Implementation uses `zmq_poll` with
 * remaining budget before each `zmq_msg_send` / `zmq_msg_recv`, so a
 * stalled peer or blocked HWM cannot burn more than the caller's
 * declared budget across the whole handshake.
 *
 * # Ownership
 *
 * `ZmqAttachChannel` does NOT own the ZMQ socket.  The caller
 * (broker's REG_REQ handler, role's REG_REQ initiator, or the future
 * `ZmqAttachProtocolAcceptor`) owns the socket via cppzmq
 * (`zmq::socket_t`) and releases the channel before disturbing the
 * socket.  This matches `ShmAttachChannel` semantics — the channel
 * is a pass-through view over the transport.
 *
 * # Contrast with `ShmAttachChannel`
 *
 * - **Peer identity** — SHM uses `SO_PEERCRED.uid`; ZMQ uses
 *   ZAP-verified CURVE pubkey (retrieved from `zmq_msg_gets(..., "User-Id")`
 *   or the routing frame).  Both verified BEFORE the channel is
 *   constructed — the channel doesn't re-check identity per frame.
 * - **Fd handoff vs. reply-and-close** — SHM's caller keeps the fd
 *   post-handshake for `SCM_RIGHTS`; ZMQ's caller sends a final
 *   response (channel_url, error, or ack) and closes the routing
 *   identity by not addressing it again.  Neither semantic lives on
 *   the channel — both are the caller's responsibility.
 */

#include "plh_platform.hpp"
#include "pylabhub_utils_export.h"
#include "utils/security/attach_channel.hpp"

#include "cppzmq/zmq.hpp"

#include <chrono>
#include <string>

namespace pylabhub::utils::security
{

/// ZMQ binding of `IAttachChannel`.  See file docstring for the wire
/// format (ROUTER vs DEALER multipart shape), deadline discipline,
/// and socket-ownership contract.
class PYLABHUB_UTILS_EXPORT ZmqAttachChannel final : public IAttachChannel
{
public:
    /// Construct a channel over an existing ZMQ socket.
    ///
    /// @param socket             Non-owning reference to a ZMQ socket
    ///                           (must remain valid for the lifetime
    ///                           of the channel).  Typically a ROUTER
    ///                           on the acceptor side or a DEALER on
    ///                           the initiator side.
    /// @param routing_identity   Peer's ROUTER routing identity for
    ///                           the ROUTER side.  Pass an empty
    ///                           string on the DEALER side (channel
    ///                           auto-selects single-frame shape).
    /// @param side               Diagnostic tag ("producer" /
    ///                           "consumer" / etc.) for error
    ///                           messages.  Not part of the wire
    ///                           protocol.  Stored by copy — safe to
    ///                           pass a temporary string.
    ZmqAttachChannel(zmq::socket_t &socket,
                     std::string    routing_identity,
                     std::string    side)
        : socket_(socket),
          routing_identity_(std::move(routing_identity)),
          side_(std::move(side))
    {}

    ~ZmqAttachChannel() override = default;

    ZmqAttachChannel(const ZmqAttachChannel &)            = delete;
    ZmqAttachChannel &operator=(const ZmqAttachChannel &) = delete;
    ZmqAttachChannel(ZmqAttachChannel &&)                 = delete;
    ZmqAttachChannel &operator=(ZmqAttachChannel &&)      = delete;

    void
    send_frame(const nlohmann::json                  &frame,
               std::chrono::steady_clock::time_point  deadline) override;

    nlohmann::json
    recv_frame(std::chrono::steady_clock::time_point deadline) override;

    /// The stored routing identity (non-owning view).  Exposed only
    /// for post-handshake follow-up (e.g. sending a channel_url
    /// response back to the same peer after successful auth).  Empty
    /// on the DEALER side.
    [[nodiscard]] const std::string &
    routing_identity() const noexcept { return routing_identity_; }

private:
    zmq::socket_t &socket_;
    std::string    routing_identity_;
    std::string    side_;
};

} // namespace pylabhub::utils::security
