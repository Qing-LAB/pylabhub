#pragma once
/**
 * @file shm_attach_orchestrator.hpp
 * @brief Composes L1 transport + L2 AttachProtocol + broker pre-confirm
 *        for the producer's accept→handshake→pre-confirm→send-fd loop
 *        (HEP-CORE-0041 §9 D4 steps 6-7, substep 1e).
 *
 * What this layer adds on top of `attach_protocol.hpp`:
 *
 *   - Calls into the broker (via injected `BrokerQuery` callback) to
 *     pre-confirm the consumer's pubkey against the channel's
 *     authoritative allowlist — the actual gate per HEP-CORE-0041 §9
 *     D4 (read-only against `ChannelAccessIndex` per substep 1d).
 *   - Computes the cache-vs-broker divergence-WARN table (HEP-0041 §9
 *     D4 "cached-allowlist semantics") and emits `LOGGER_WARN` on
 *     either divergence direction.
 *   - On broker success → calls `transport.send_capability(peer_fd)`
 *     to hand the SHM fd to the consumer via `SCM_RIGHTS`.
 *   - On broker denied → drops the peer socket (no fd handed over).
 *   - On any error path (broker comm failure, MAC verification failure
 *     in the handshake, send_capability failure) → fails CLOSED:
 *     peer socket dropped, no fd handed over.
 *
 * **Divergence table** (HEP-CORE-0041 §9 D4):
 *
 *     | cache says  | broker says | log                     | action |
 *     |-------------|-------------|-------------------------|--------|
 *     | allowed     | success     | silent (healthy)        | admit  |
 *     | denied      | denied      | silent (healthy)        | deny   |
 *     | denied      | success     | WARN; admit; investigate| admit  |
 *     | allowed     | denied      | WARN; deny; investigate | deny   |
 *
 *   The broker is always authoritative.  Sustained divergence rate is
 *   the broker-NOTIFY-pipeline health metric operators monitor.
 *
 * **Injected callbacks** (clean separation from production wiring):
 *
 *   - `CacheLookup(consumer_pubkey)` — production wires to the
 *     channel's `PeerAllowlist::contains` (populated via
 *     `CHANNEL_AUTH_CHANGED_NOTIFY` doorbell + `GET_CHANNEL_AUTH_REQ`
 *     pull per HEP-CORE-0036 §6.5).  Tests inject a controlled bool.
 *
 *   - `BrokerQuery(consumer_pubkey, consumer_role_uid)` — production
 *     wires to `BrokerRequestComm::consumer_attach()` (shipped in
 *     substep 1d).  Tests inject a fake reply.
 *
 * The class never touches `BrokerRequestComm` or `PeerAllowlist`
 * directly — keeps L2 testable and the L1/L2 layering clean.
 *
 * @see docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md §9 D4 (attach
 *      sequence + cached-allowlist semantics tables).
 * @see attach_protocol.hpp for the consumer→producer handshake half.
 * @see broker_request_comm.hpp BrokerRequestComm::consumer_attach
 *      for the production broker-query implementation.
 */

#include "plh_platform.hpp"
#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"
#include "utils/security/attach_protocol.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace pylabhub::utils::security
{

#if defined(PYLABHUB_PLATFORM_LINUX)

/// Orchestrator owns the per-accept lifecycle but holds REFERENCES
/// to the L1 transport + L2 acceptor — both must outlive this
/// instance.  Designed for the producer's accept loop: construct
/// once, call `accept_and_serve_one()` in a loop until shutdown.
class PYLABHUB_UTILS_EXPORT ShmAttachOrchestrator
{
public:
    /// Callback: is this consumer_pubkey in the producer's local
    /// cached allowlist for the channel?
    using CacheLookup = std::function<bool(const std::string &consumer_pubkey)>;

    /// Callback: send a CONSUMER_ATTACH_REQ_SHM to the broker and return
    /// the reply body (or `nullopt` on transport timeout / connection
    /// loss).  Reply shape is whatever `BRC::consumer_attach` returns:
    ///   {status: "success" | "denied", denial_reason?: ..., ...}
    using BrokerQuery = std::function<std::optional<nlohmann::json>(
        const std::string &consumer_pubkey,
        const std::string &consumer_role_uid)>;

    struct Config
    {
        std::string channel_name;
        std::string producer_role_uid;
        CacheLookup cache_lookup;
        BrokerQuery broker_query;
    };

    /// Discrete outcomes — tests pin specific outcomes per scenario;
    /// production code can branch on these for metrics / structured
    /// logging beyond what the orchestrator emits internally.
    enum class Outcome
    {
        Sent,                 ///< Capability fd handed to consumer.
        DeniedByBroker,       ///< Broker replied status="denied"; peer dropped.
        DeniedTransportFail,  ///< Broker comm failure OR send_capability
                              ///< failure; fails CLOSED (peer dropped, no
                              ///< fd handed over).
        HandshakeFailed,      ///< AttachProtocolAcceptor threw (MAC
                              ///< failure, bad hello shape, etc.); peer
                              ///< already closed by the acceptor.
        Timeout,              ///< No consumer connected within `timeout`.
    };

    /// @param acceptor   L2 hello + challenge-response handler (must
    ///                   outlive this orchestrator).
    /// @param transport  L1 transport (must outlive this orchestrator).
    /// @param config     Channel context + injected callbacks.
    ShmAttachOrchestrator(AttachProtocolAcceptor &acceptor,
                          IShmCapabilityProducer &transport,
                          Config                  config);

    ~ShmAttachOrchestrator() = default;

    ShmAttachOrchestrator(const ShmAttachOrchestrator &)            = delete;
    ShmAttachOrchestrator &operator=(const ShmAttachOrchestrator &) = delete;
    ShmAttachOrchestrator(ShmAttachOrchestrator &&)                 = delete;
    ShmAttachOrchestrator &operator=(ShmAttachOrchestrator &&)      = delete;

    /// One pass through the accept→handshake→pre-confirm→send-fd flow.
    /// Returns `Timeout` if no consumer connects within `timeout`.
    /// On `Sent` / `DeniedByBroker`, the broker's verdict is logged at
    /// INFO with `event=AttachAuthorized` or `event=AttachDenied`
    /// markers (#238 stable-marker format).  On any divergence between
    /// cache and broker, a `LOGGER_WARN` fires with structured
    /// `divergence=` payload.
    ///
    /// All failure paths are fail-closed: the peer socket is closed
    /// before this function returns anything other than `Sent`.
    Outcome
    accept_and_serve_one(std::chrono::milliseconds timeout);

private:
    AttachProtocolAcceptor &acceptor_;
    IShmCapabilityProducer &transport_;
    Config                  config_;
};

#endif // PYLABHUB_PLATFORM_LINUX

} // namespace pylabhub::utils::security
