#pragma once
/**
 * @file attach_channel.hpp
 * @brief Transport-agnostic frame-carrying channel used by the
 *        AttachProtocol challenge-response (HEP-CORE-0043 §6 +
 *        HEP-CORE-0041 §9 D4).
 *
 * @author  pyLabHub team
 * @date    2026-07-07 (Phase 3 — interface + SHM binding;
 *                      Phase 4a — ZMQ binding)
 * @copyright  MIT
 *
 * # Purpose
 *
 * `IAttachChannel` is the seam between the AttachProtocol
 * challenge-response CRYPTOGRAPHIC logic (transport-agnostic, lives
 * in `attach_protocol.cpp`) and the underlying wire TRANSPORT
 * (SHM AF_UNIX socket, ZMQ ROUTER, or future backends).
 *
 * The protocol code sends/receives a JSON frame per step; the
 * channel implementation handles the actual byte-level framing +
 * deadline propagation over its transport:
 *
 *   - `ShmAttachChannel` (Linux only, Phase 3) — length-prefixed
 *     JSON over an AF_UNIX SOCK_STREAM fd.  4-byte little-endian
 *     length prefix + JSON body, 4 KiB max per frame.  Peer identity
 *     verified out-of-band via `SO_PEERCRED` (uid match).  On
 *     success, the caller retains the fd for a subsequent
 *     SCM_RIGHTS handoff.
 *
 *   - `ZmqAttachChannel` (Phase 4a, SHIPPED 2026-07-07) — JSON body
 *     carried inside a ZMQ multipart message.  Two-part shape on
 *     the ROUTER side: `[routing_id][json_body]` (channel
 *     auto-prepends the routing frame + auto-verifies on recv
 *     against the peer identity stored at construction).
 *     Single-part shape on the DEALER side: `[json_body]` (no
 *     routing identity needed because DEALER's connection is 1:1).
 *     Peer identity verified out-of-band via ZAP-validated CURVE
 *     pubkey.  On success, the caller responds with a channel_url
 *     message and stops addressing the routing identity.
 *
 * Phase 4b (future) will extract the AttachProtocol producer-side
 * handshake body into a transport-agnostic
 * `run_producer_handshake(IAttachChannel &, ...)` helper — currently
 * `AttachProtocolAcceptor::accept_one` retains SHM-specific SO_PEERCRED
 * uid + SCM_RIGHTS fd-handoff logic surrounding the channel usage,
 * which the ZMQ acceptor will factor out.
 *
 * # Contract
 *
 * A channel is per-peer: each accept produces a new `IAttachChannel`
 * instance that carries the identity of ONE peer through the
 * handshake.  The protocol code owns the channel for the duration of
 * the handshake; the transport-specific driver (`AttachProtocolAcceptor`,
 * future `ZmqAttachProtocolAcceptor`) creates it, hands it to the
 * protocol, and reclaims it on completion (success handoff or error
 * close).
 *
 * Implementations MUST be non-copyable AND non-movable — a channel
 * ties to a specific fd/socket that outlives it, and moving the
 * channel could dangle the underlying reference.  Callers hold
 * channels by value on the stack for the handshake's duration.
 *
 * Both `send_frame` and `recv_frame` respect a caller-supplied
 * absolute deadline so multi-step handshakes are bounded end-to-end.
 * On timeout, both throw `AttachProtocolTimeout` — the protocol code
 * catches this specifically to distinguish "peer stalled" from
 * "protocol violation."
 *
 * On framing / JSON / size-limit errors, both throw
 * `std::runtime_error` (protocol violation — do not retry).
 */

#include "plh_platform.hpp"
#include "pylabhub_utils_export.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include "utils/json_fwd.hpp"
#include <stdexcept>
#include <string>

namespace pylabhub::utils::security
{

/// Transport-agnostic frame-carrying channel for the AttachProtocol
/// challenge-response.  See file docstring for the design rationale.
///
/// Implementations are non-copyable AND non-movable.  Implementations
/// are NOT thread-safe — a single peer's frames are exchanged
/// sequentially by the protocol code.
class PYLABHUB_UTILS_EXPORT IAttachChannel
{
public:
    /// Maximum JSON frame size accepted by any conforming
    /// implementation.  Same value as the SHM channel's historical
    /// cap (`attach_protocol.cpp` pre-Phase-3); every transport uses
    /// the same bound so DoS behaviour is uniform.
    static constexpr std::size_t kMaxAttachFrameBytes = 4096;

    virtual ~IAttachChannel() = default;

    /// Send a JSON frame to the peer.
    ///
    /// @param frame     JSON body to send.  Must not exceed
    ///                  `kMaxAttachFrameBytes` when serialized;
    ///                  larger frames throw `std::runtime_error` before
    ///                  any bytes reach the wire.
    /// @param deadline  Absolute time by which the send must complete.
    /// @throws AttachProtocolTimeout  if the deadline is reached
    ///                                mid-send (peer stalled).
    /// @throws std::runtime_error     on transport error, oversized
    ///                                frame, or peer disconnect.
    virtual void
    send_frame(const nlohmann::json &frame,
               std::chrono::steady_clock::time_point deadline) = 0;

    /// Receive a JSON frame from the peer.
    ///
    /// @param deadline  Absolute time by which a full frame must
    ///                  arrive.
    /// @return          Parsed JSON frame.
    /// @throws AttachProtocolTimeout  if the deadline is reached
    ///                                before a full frame arrives.
    /// @throws std::runtime_error     on transport error, malformed
    ///                                length prefix, oversized frame,
    ///                                bad JSON, or peer disconnect.
    virtual nlohmann::json
    recv_frame(std::chrono::steady_clock::time_point deadline) = 0;
};

// ─────────────────────────────────────────────────────────────────
// Shared utilities used by every `IAttachChannel` implementation.
// Inline in the header to avoid a separate util TU while giving all
// callers (channel impls AND `attach_protocol.cpp`'s socket setup)
// one canonical version of each helper.  Previously duplicated in
// `attach_channel_shm.cpp` + `attach_channel_zmq.cpp` +
// `attach_protocol.cpp` (Phase 3 residue, hoisted 2026-07-07).
// ─────────────────────────────────────────────────────────────────

/// Compute remaining budget from an absolute deadline; zero if past.
/// Used to bound each recv/send call against a SHARED deadline so an
/// entire multi-call handshake step is bounded at exactly `timeout` —
/// not `N * timeout`.
[[nodiscard]] inline std::chrono::milliseconds
attach_remaining_ms(std::chrono::steady_clock::time_point deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::chrono::milliseconds{0};
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

/// Build a `std::runtime_error` with the canonical AttachProtocol
/// message shape: `"AttachProtocol::<side>: <what>: <strerror>"`.
[[nodiscard]] inline std::runtime_error
attach_make_errno_error(const char *side, const char *what,
                        int captured_errno)
{
    return std::runtime_error(std::string{"AttachProtocol::"} + side + ": " +
                              what + ": " + std::strerror(captured_errno));
}

} // namespace pylabhub::utils::security
