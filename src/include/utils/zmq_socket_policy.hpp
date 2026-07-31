#pragma once
/**
 * @file zmq_socket_policy.hpp
 * @brief Centralized ZMQ socket-option policy for pylabhub.
 *
 * pylabhub policy (audit S1, 2026-05-18): **disconnect is terminal**
 * for any ZMQ socket that crosses process or host boundaries.  We run
 * on reliable local networks and treat a broker / peer disconnect as a
 * lifecycle event — the role / hub / queue must explicitly decide to
 * restart, not wait for libzmq to silently auto-reconnect to a peer
 * that may not be the same process anymore.
 *
 * This header centralizes the socket-option block that implements the
 * policy so every ZMQ-using subsystem (BRC, ZmqQueue, InboxQueue,
 * future federation transports) applies it uniformly.  Future
 * developer adding a new ZMQ socket: call `apply_socket_policy(...)`
 * immediately after `socket_t.emplace(...)` and before `bind()` /
 * `connect()`.
 *
 * Full policy contract: `docs/HEP/HEP-CORE-0023-Startup-Coordination.md`
 *                       §2.5.3 "Disconnection is terminal"
 *                       and `docs/IMPLEMENTATION_GUIDANCE.md`
 *                       § "Role-side ZMQ socket policy".
 *
 * Layer-1 ("connection-state gate") and layer-3 ("application-level
 * reply timeout") of the 4-layer model are NOT set here — they live in
 * each subsystem (BRC uses `pImpl->connected` + per-method
 * `timeout_ms`; ZmqQueue/Inbox have their own equivalents).  This
 * helper covers ONLY the socket-option layer (layer 2 send timeout +
 * layer 4 ZMTP heartbeat + auto-reconnect disable).
 */

#include "cppzmq/zmq.hpp"

#include <cstddef>
#include <initializer_list>

namespace pylabhub::utils
{

/// Role of a ZMQ socket in pylabhub.  Selects which options apply.
enum class ZmqSocketRole
{
    /// Connect-side TCP socket — initiates the connection.  Examples:
    ///   - BRC's DEALER connecting to a broker ROUTER
    ///   - ZmqQueue PUSH (connect variant) sending to a remote PULL
    ///   - Inbox sender DEALER connecting to a peer ROUTER
    /// Gets: linger, sndtimeo, heartbeat, AND reconnect-disable.
    TcpConnect,

    /// Bind-side TCP socket — accepts connections.  Examples:
    ///   - Broker's ROUTER accepting DEALER connections
    ///   - ZmqQueue PULL (bind variant) accepting PUSH senders
    ///   - Inbox receiver ROUTER accepting DEALER senders
    /// Gets: linger, sndtimeo, heartbeat.  No reconnect (bind sockets
    /// don't initiate connections — option would be a no-op).
    TcpBind,

    /// Inproc PAIR for intra-process signaling (e.g. wake-up).
    /// Gets: linger only.  No heartbeat (irrelevant for inproc — ZMTP
    /// heartbeat is a TCP transport concern).  No sndtimeo (inproc
    /// sub-microsecond — can't meaningfully bound).  No reconnect
    /// (inproc isn't subject to TCP teardown semantics).
    InprocSignal,
};

/// Apply pylabhub's standard socket-option policy to `sock`.
///
/// MUST be called immediately after `socket_t.emplace(ctx, type)` and
/// BEFORE `bind()` / `connect()` — some libzmq options
/// (`ZMQ_RECONNECT_IVL` in particular) only take effect when set
/// pre-connect.  Caller may set additional subsystem-specific options
/// (`ZMQ_ROUTING_ID`, `ZMQ_SUBSCRIBE`, CURVE keys, HWMs, etc.) BEFORE
/// or AFTER this call as appropriate.
///
/// Policy values (audit S1, 2026-05-18):
///   linger           = 0       Clean shutdown — drop unsent messages
///                              on socket close; never block teardown.
///   sndtimeo         = 500 ms  Layer 2 of the 4-layer time-bound
///                              model (HEP-0023 §2.5.3).  Bounds
///                              send-side block so poll threads can
///                              observe stop_requested promptly even
///                              if libzmq's internal queue is
///                              saturated.  Send returns EAGAIN on
///                              timeout; callers must handle (BRC
///                              wraps send in try/catch, logs WARN).
///   heartbeat_ivl    = 5 s     ZMTP-level keep-alive (layer 4).
///   heartbeat_timeout= 30 s    If no PONG within timeout, ZMTP tears
///                              the connection (ZMQ_EVENT_DISCONNECTED
///                              fires exactly once).
///   reconnect_*                Selected as a SET by the build-time
///                              posture `PYLABHUB_ZMQ_RECONNECT_POLICY`
///                              (cmake/ToplevelOptions.cmake).  See the
///                              block below — the distinction it encodes
///                              is "never established" vs "established
///                              then lost", which is NOT the same thing
///                              and is not settable with one option.
inline void apply_socket_policy(zmq::socket_t &sock, ZmqSocketRole role)
{
    // Always: clean shutdown.
    sock.set(zmq::sockopt::linger, 0);

    if (role == ZmqSocketRole::InprocSignal)
        return;

    // Bounded send-side block (layer 2).  Sends to a saturated queue
    // or stuck transport return EAGAIN after 500 ms.
    sock.set(zmq::sockopt::sndtimeo, 500);

    // ZMTP keep-alive (layer 4) — TCP-only; no-op on inproc/ipc.
    sock.set(zmq::sockopt::heartbeat_ivl, 5000);
    sock.set(zmq::sockopt::heartbeat_timeout, 30000);

    if (role == ZmqSocketRole::TcpConnect)
    {
        // ── Reconnect posture ────────────────────────────────────────────
        // Only meaningful on connect-side sockets — bind-side never
        // initiates, so these options are no-ops there.
        //
        // READ THIS BEFORE CHANGING: these options govern ONLY a connection
        // that was never established.  An ESTABLISHED session being lost is
        // not reachable from here — `ZMQ_RECONNECT_STOP_AFTER_DISCONNECT`
        // fires inside `socket_base_t::term_endpoint`, i.e. when WE call
        // `zmq_disconnect()`, not when a peer drops.  libzmq has no flag for
        // peer-initiated loss of a live connection.  "Disconnect is terminal"
        // for a live session (HEP-CORE-0023 §2.5.3) is therefore enforced
        // ABOVE the socket, by watching `ZMQ_EVENT_DISCONNECTED` on the
        // monitor and tearing the socket down — task #93.  Setting a flag
        // here does not give you that property, however it is named.
#if defined(PYLABHUB_ZMQ_RECONNECT_TERMINAL)
        sock.set(zmq::sockopt::reconnect_ivl, -1); // never reconnect
        sock.set(zmq::sockopt::reconnect_ivl_max, 0);
#else
        // startup-tolerant (default).  Retrying a connection that never
        // completed carries no protocol-state risk — there is no session to
        // corrupt — and it covers the real ordering race where a role starts
        // before the broker is listening.  Bounded backoff so a peer that
        // never appears is not hammered.
        sock.set(zmq::sockopt::reconnect_ivl, 100);
        sock.set(zmq::sockopt::reconnect_ivl_max, 1000);
#endif

        // Unconditional in BOTH postures, and deliberately not switchable:
        // stop retrying when the answer is already known.  ECONNREFUSED
        // means nothing is listening; a failed handshake means we are not
        // authorised or not speaking the same protocol.  Retrying either is
        // pure noise — and libzmq's raw default (which is what any socket
        // that never called this function gets) does exactly that, ten times
        // a second, forever.  DRAFT option; guarded so a non-draft libzmq
        // still compiles.
#if defined(ZMQ_RECONNECT_STOP)
        sock.set(zmq::sockopt::reconnect_stop,
                 ZMQ_RECONNECT_STOP_CONN_REFUSED | ZMQ_RECONNECT_STOP_HANDSHAKE_FAILED |
                     ZMQ_RECONNECT_STOP_AFTER_DISCONNECT);
#endif
    }
}


/// Submit `parts` as ONE multipart message without blocking, and without
/// leaving the socket's multipart state armed on failure.
///
/// **Why this is not `zmq::multipart_t::send`.**  libzmq refuses to put half a
/// message on the wire: when a NON-FIRST part fails to write it rolls back
/// what it wrote and arms a *discard mode* that silently swallows every
/// following part **while reporting success**, until one arrives without
/// SNDMORE (`lb_t::sendpipe`).  A caller that simply stops on failure leaves
/// that latch armed, and it then consumes the caller's NEXT message whole and
/// reports it as sent.  `multipart_t::send` returns only a bool, so a caller
/// using it cannot tell which part failed and therefore cannot clear the latch.
///
/// The distinction is load-bearing in BOTH directions:
///   - first part failed  → libzmq wrote nothing and the latch is NOT armed;
///     flushing the remainder here would queue orphan parts that emerge as a
///     malformed message the moment a peer appears;
///   - later part failed  → the latch IS armed; the remaining parts must be
///     pushed so it clears inside THIS message rather than eating the next one.
///
/// Lives here, with the socket-option policy, because it is the same kind of
/// thing: a house rule for how pylabhub uses a ZMQ socket, applying to any
/// multipart sender rather than to one message format.
///
/// @return true if every part was accepted; false if the socket had no
///         writable peer (receiver at its high-water mark, or no peer at all).
///         Throws `zmq::error_t` for any non-EAGAIN transport error, matching
///         cppzmq's convention.
[[nodiscard]] inline bool send_multipart_atomic(zmq::socket_t &sock,
                                                std::initializer_list<zmq::const_buffer> parts)
{
    const std::size_t n = parts.size();
    if (n == 0)
        return true;

    std::size_t failed_at = n;
    std::size_t i = 0;
    for (const auto &part : parts)
    {
        const auto flags = (i + 1 < n) ? (zmq::send_flags::sndmore | zmq::send_flags::dontwait)
                                       : zmq::send_flags::dontwait;
        if (!sock.send(part, flags))
        {
            failed_at = i;
            break;
        }
        ++i;
    }
    if (failed_at == n)
        return true;

    // Clear libzmq's discard latch ONLY when a later part failed (see above).
    if (failed_at > 0)
    {
        std::size_t k = 0;
        for (const auto &part : parts)
        {
            if (k > failed_at)
            {
                const auto flags = (k + 1 < n)
                                       ? (zmq::send_flags::sndmore | zmq::send_flags::dontwait)
                                       : zmq::send_flags::dontwait;
                (void)sock.send(part, flags);
            }
            ++k;
        }
    }
    return false;
}

} // namespace pylabhub::utils
