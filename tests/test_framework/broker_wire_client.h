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

        /// Optional linger override (ms).  Default 0 — matches
        /// production ROUTER teardown behaviour (drop in-flight on close).
        int linger_ms = 0;

        /// Optional SNDTIMEO / RCVTIMEO (ms).  Default 500 — matches BRC.
        int sndtimeo_ms = 500;
    };

    /// Construct + connect.  Throws `zmq::error_t` on connect failure
    /// (bad endpoint, CURVE misconfig).  CURVE handshake happens
    /// asynchronously — call `wait_ready` to synchronize on it.
    explicit BrokerWireClient(zmq::context_t& ctx, const Config& cfg);
    ~BrokerWireClient();

    BrokerWireClient(const BrokerWireClient&)            = delete;
    BrokerWireClient& operator=(const BrokerWireClient&) = delete;

    /// Fire-and-forget send of the 3-frame wire `[C, msg_type, body]`.
    /// Returns immediately; DEALER queues locally and libzmq handles the
    /// send.  Throws on send failure (should not happen once connected).
    void send(std::string_view msg_type, const nlohmann::json& body);

    /// Poll for the next reply frame with a deadline.
    /// - Returns the `{msg_type, body}` pair on success.
    /// - Returns `nullopt` on timeout.
    /// - Throws on protocol error (wrong frame count, non-string msg_type,
    ///   invalid JSON body).
    ///
    /// Note: reads whatever the broker sent next — a request-reply test
    /// that expects a specific ACK type must either use `request()` below
    /// or filter unwanted NOTIFY frames itself.
    [[nodiscard]] std::optional<std::pair<std::string, nlohmann::json>>
    receive(std::chrono::milliseconds timeout);

    /// Convenience for the common request-reply shape: send `req_type`
    /// with `body`, then poll receive() until either the expected
    /// `expect_ack_type` arrives (returned) or `timeout` elapses
    /// (`nullopt`).  Discards other message types silently — use for
    /// tests that don't care about unsolicited NOTIFY traffic.
    ///
    /// Timeout is applied to the WHOLE call (send + poll loop), matching
    /// the production BRC sync-REQ semantics.
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
