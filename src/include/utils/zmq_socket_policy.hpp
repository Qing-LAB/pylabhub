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
///   reconnect_ivl    = -1      Auto-reconnect DISABLED.  Pylabhub
///                              policy: disconnect is terminal.  A
///                              "reconnected" socket may be talking
///                              to a different process that doesn't
///                              know our role's state — reusing it
///                              would corrupt the protocol layer.
///                              Lifecycle-layer restart required for
///                              re-establishment.
///   reconnect_ivl_max= 0       Belt: even if a future edit re-enables
///                              reconnect_ivl, no exp-backoff escape.
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
        // Auto-reconnect DISABLED (layer 1 enabler).  Only meaningful
        // on connect-side sockets — bind-side doesn't initiate.
        sock.set(zmq::sockopt::reconnect_ivl, -1);
        sock.set(zmq::sockopt::reconnect_ivl_max, 0);
    }
}

} // namespace pylabhub::utils
