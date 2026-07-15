#pragma once
/**
 * @file broker_wire_client.h
 * @brief Raw ZMQ DEALER wire-protocol client for L3 broker-only tests.
 *
 * Purpose (HEP-CORE-0042 Phase 2.4a).  Existing Pattern4 tests exercise
 * broker+role interaction — a full role subprocess speaks BrokerRequestComm
 * against a broker subprocess.  That harness is too heavy for tests whose
 * intent is to verify the BROKER'S wire behaviour in isolation (e.g. the
 * HEP-0042 attach-coordination handlers).  BrokerWireClient is the light
 * substitute: a raw DEALER socket that speaks the 3-frame wire directly
 * (`[C, msg_type, body]`), lets tests inject arbitrary REG_REQ /
 * CONSUMER_ATTACH_REQ_ZMQ / CHANNEL_AUTH_APPLIED_REQ traffic, and observe
 * the broker's replies + unsolicited NOTIFY frames.
 *
 * Design constraints:
 *
 *   - **CURVE-mandatory.**  Broker ROUTER binds with CURVE + ZAP per
 *     HEP-CORE-0036 §7.4.  Client MUST configure `curve_serverkey` /
 *     `curve_publickey` / `curve_secretkey` before connect, otherwise
 *     the handshake fails silently at ZAP.
 *   - **DEALER, not REQ.**  Matches production BrokerRequestComm shape;
 *     lets a single client send multiple requests without waiting for
 *     replies (needed for e.g. wait-path tests: send ATTACH_REQ, expect
 *     no immediate reply, send APPLIED_REQ, expect BOTH replies).
 *   - **No socket monitor.**  Tests use `receive` timeout as the sole
 *     health signal; monitor would add threading complexity for no
 *     value at test scope.
 *   - **Reconnect disabled** (`reconnect_ivl = -1`).  Matches production
 *     BRC per HEP-CORE-0023 §2.5.3 "disconnection is terminal": tests
 *     that kill and respawn the broker MUST re-observe the disconnect
 *     as a receive-side timeout rather than silently reconnecting.
 *   - **KeyStore-independent.**  The client takes raw z85 pubkey/seckey
 *     pairs in the config so callers can construct a client without
 *     touching the process KeyStore (a test may want two clients with
 *     different identities in the same test process — KeyStore's single
 *     `"role_identity"` slot doesn't support that shape).
 *
 * Wire frame layout (matches BrokerRequestComm + BrokerService):
 *
 *     Frame 0: single byte 'C' (kFrameTypeControl)
 *     Frame 1: msg_type string (e.g. "REG_REQ")
 *     Frame 2: JSON body
 *
 * The DEALER<->ROUTER pair strips/prepends the routing-identity frame
 * transparently; the client does not see it on receive.
 */

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <cppzmq/zmq.hpp>
#include <nlohmann/json.hpp>

namespace pylabhub::tests::pattern4 {

class BrokerWireClient
{
public:
    struct Config
    {
        /// "tcp://127.0.0.1:PORT" — where the broker's ROUTER binds.
        std::string broker_endpoint;

        /// Broker's CURVE public key (z85, 40 chars) — the client's
        /// `curve_serverkey` for the DEALER connect.
        std::string broker_pubkey;

        /// Client's own CURVE keypair (z85, 40 chars each).  Distinct
        /// from the broker's; matches what a role would present at ZAP.
        std::string client_pubkey;
        std::string client_seckey;

        /// DEALER routing_id (I-DEALER-IDENTITY, HEP-CORE-0046 §8.1).
        /// Set to the role_uid this wire client masquerades as;
        /// broker's envelope parse uses it as Frame 0 identity so
        /// envelope_hash reconstructs against a stable per-connection
        /// value.  REQUIRED — construction throws if empty per
        /// I-DEALER-IDENTITY (the "pattern4-wire-client" default was
        /// a sentinel that let multiple test clients collide on the
        /// same ROUTER identity, silently reusing routing state
        /// across independent tests).
        std::string client_role_uid;

        /// Optional linger override (ms).  Default 0 — matches production
        /// BRC's DEALER teardown (drop in-flight per HEP-CORE-0023
        /// §2.5.3 "disconnection is terminal"; the client mirrors this
        /// even though "test kills client mid-send" is not a real scenario).
        int linger_ms = 0;

        /// Optional SNDTIMEO (ms).  Default 500 — matches BRC's
        /// `apply_socket_policy` send budget so `send()` cannot block
        /// indefinitely under HWM contention.  RCVTIMEO is intentionally
        /// NOT set (receive() enforces its own budget via zmq::poll +
        /// dontwait — see receive() docstring).
        int sndtimeo_ms = 500;
    };

    /// Construct + connect the DEALER socket.  CURVE options are applied
    /// then `dealer.connect()` is called; both are non-blocking for TCP
    /// endpoints — the connect returns after socket setup without
    /// validating that the broker is reachable or that CURVE will
    /// succeed.  A misconfigured server key surfaces later as a
    /// silent ZAP-side handshake failure (per class docblock line 21),
    /// observed as a `receive()` timeout.  Tests that want to gate on
    /// "handshake actually succeeded" should send a lightweight REQ
    /// (e.g. HEARTBEAT_NOTIFY) and treat a matching reply as the ready
    /// signal — there is no dedicated `wait_ready` helper.
    ///
    /// The only cases that throw at construct time are libzmq's
    /// setsockopt validation of a malformed z85 CURVE key (throws
    /// `zmq::error_t` from `dealer.set(...)`).
    explicit BrokerWireClient(zmq::context_t& ctx, const Config& cfg);
    ~BrokerWireClient();

    // Movable (pImpl is unique_ptr; no additional invariants to
    // preserve across move).  Copies are disabled because a DEALER
    // socket cannot be duplicated.  Rule-of-five: user-declared dtor
    // implicitly deletes the move members, so we explicitly re-enable.
    BrokerWireClient(BrokerWireClient&&) noexcept            = default;
    BrokerWireClient& operator=(BrokerWireClient&&) noexcept = default;
    BrokerWireClient(const BrokerWireClient&)                = delete;
    BrokerWireClient& operator=(const BrokerWireClient&)     = delete;

    /// Send the 3-frame wire `[C, msg_type, body]` on the DEALER socket.
    /// Semantics: `zmq::send_multipart` with default blocking flags,
    /// bounded by `Config::sndtimeo_ms` (default 500 ms).  On a normally-
    /// connected socket this returns in microseconds; under HWM
    /// contention or a socket that has not yet completed the CURVE
    /// handshake it MAY block up to `sndtimeo_ms` and then throw
    /// `zmq::error_t` (EAGAIN).  Tests that spam sends (e.g.
    /// wait-path queueing stress) should either drain replies between
    /// bursts or catch the exception.
    void send(std::string_view msg_type, const nlohmann::json& body);

    /// Poll for the next reply frame with a deadline.
    /// - Returns the `{msg_type, body}` pair on success.
    /// - Returns `nullopt` on timeout (`timeout` <= 0 is treated as 0
    ///   — non-blocking check — to avoid the negative-value hang in
    ///   `zmq_poll`).
    /// - Throws `std::runtime_error` on protocol violation (frame
    ///   count != 3, missing 'C' control byte, or JSON parse failure
    ///   on the body).  The msg_type frame is passed through as bytes
    ///   without a "is-string" validation — the wire is octet-typed
    ///   and downstream comparison catches malformed types via a
    ///   mismatch.
    ///
    /// Reads whatever the broker sent next — a request-reply test
    /// that expects a specific ACK type must either use `request()`
    /// below or filter unwanted NOTIFY frames itself.
    [[nodiscard]] std::optional<std::pair<std::string, nlohmann::json>>
    receive(std::chrono::milliseconds timeout);

    /// Convenience for the common request-reply shape: send `req_type`
    /// with `body`, then poll receive() until either the expected
    /// `expect_ack_type` arrives, or an `ERROR` frame arrives (broker
    /// rejected the request; returned so the caller sees the reason
    /// instead of burning the whole budget), or `timeout` elapses
    /// (`nullopt`).  Other message types (e.g. unsolicited NOTIFY)
    /// are silently discarded so tests that only care about the
    /// request-reply pair don't need to filter them.
    ///
    /// If the broker returned ERROR, the reply body is the ERROR
    /// payload; the caller distinguishes success vs error by
    /// inspecting `body.value("status", "")` or the presence of an
    /// `error_code` field.  Callers that need to see the raw
    /// `msg_type` should call `send()` + `receive()` directly.
    ///
    /// Timeout is applied to the WHOLE call (send + poll loop),
    /// matching the production BRC sync-REQ semantics.  A
    /// `zmq::error_t` from the underlying `send()` (SNDTIMEO under
    /// HWM contention) propagates — this is by design because such a
    /// failure is not a "timeout" outcome but a socket-state error
    /// the test should surface immediately.
    [[nodiscard]] std::optional<nlohmann::json>
    request(std::string_view      req_type,
             const nlohmann::json &body,
             std::string_view      expect_ack_type,
             std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub::tests::pattern4
