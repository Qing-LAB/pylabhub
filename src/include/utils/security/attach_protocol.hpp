#pragma once
/**
 * @file attach_protocol.hpp
 * @brief Producer-side L2 attach protocol for HEP-CORE-0041 SHM channels.
 *
 * Layered above the L1 IShmCapabilityProducer / Consumer abstractions
 * (`shm_capability_channel.hpp`):
 *   - **L1** = transport mechanics (memfd_create + AF_UNIX socket
 *     + SCM_RIGHTS fd handoff).
 *   - **L2 (this file)** = auth orchestration:
 *       1. SO_PEERCRED uid sanity (defence-in-depth same-uid check
 *          per HEP-CORE-0036 §I8).
 *       2. Cryptographic challenge-response proving the consumer
 *          holds the seckey corresponding to its claimed pubkey
 *          (libsodium `crypto_box` based — see flow below).
 *
 * **Challenge-response flow** (HEP-CORE-0041 §9 D4, amended 2026-06-17):
 *
 *   Frame 1, producer→consumer (length-prefixed JSON):
 *     `{protocol_version, nonce_b64, challenge_b64}`
 *
 *   Frame 2, consumer→producer (length-prefixed JSON):
 *     `{protocol_version, role_uid, pubkey_z85,
 *       challenge_response_b64}`
 *     where `challenge_response = crypto_box_easy(challenge, nonce,
 *                                                  producer_pk,
 *                                                  consumer_sk)`.
 *
 *   Producer verifies via `crypto_box_open_easy(cipher, nonce,
 *                                                consumer_pk_from_hello,
 *                                                producer_sk)` AND
 *   that the recovered plaintext equals the issued challenge.
 *
 * The MAC of `crypto_box_easy` is keyed by `ECDH(consumer_sk,
 * producer_pk)`.  A successful `crypto_box_open_easy` with
 * `consumer_pk` + `producer_sk` proves the cipher was produced by
 * the holder of `consumer_sk` corresponding to `consumer_pk` — i.e.
 * the consumer is who it claims to be.  This is the same security
 * property ZMQ's CURVE handshake provides, using the same
 * Curve25519 keypairs (no separate Ed25519 signing key needed).
 *
 * **Wire format:** every frame is a 4-byte little-endian length
 * prefix followed by a JSON body.  4 KiB DoS cap on body size.
 *
 * **Use-not-export discipline** (HEP-CORE-0043 §1.4 / §6): callers
 * identify their own identity keypair by KeyStore ENTRY NAME (e.g.
 * `kRoleIdentityName`, `kHubIdentityName`, `"broker.observer"`).
 * The seckey bytes never cross the AttachProtocol API — SMS
 * resolves the name via `keys().with_seckey(name, ...)` inside
 * `secure().box_encrypt_using(name, ...)` /
 * `secure().box_decrypt_using(name, ...)` and dereferences the
 * bytes inside the security module only.  Tests seed their
 * test-local identity into KeyStore via `seed_curve_identities()`
 * before constructing the acceptor / consumer material — same
 * production-shaped path.
 *
 * **Platform support today:** Linux only.  The non-Linux blocks in
 * the .cpp `#error` symmetrically with the L1 backend, because the
 * L2 needs the per-platform `AcceptedPeer` variant branching too —
 * tracked under tasks #259 / #260 / #261 (sibling to the L1 ports).
 *
 * **Mutual-auth gap.** This L2 surface proves consumer→producer
 * identity but does NOT prove producer→consumer.  Task #262 tracks
 * adding producer-side proof-of-possession before declaring Phase
 * 1 production-ready.
 *
 * @see docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md §9 D4.
 * @see docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md §I8.
 * @see docs/HEP/HEP-CORE-0040-Locked-Key-Memory.md §5.2 use-not-export.
 */

#include "plh_platform.hpp"
#include "pylabhub_utils_export.h"
#include "utils/security/attach_channel.hpp"        // Phase 4b — IAttachChannel
#include "utils/security/attach_channel_zmq.hpp"    // Phase 4c — ZmqAttachChannel + zmq::socket_t
#include "utils/security/secure_subsystem.hpp"      // secure() facade
#include "utils/security/shm_capability_channel.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#if defined(PYLABHUB_IS_POSIX)
#    include <sys/types.h>  // uid_t
#endif

namespace pylabhub::utils::security
{

/// HEP-CORE-0041 §D1(d) broker observer pubkey accessor (task #317 C.2.b).
/// Called by the acceptor when a Frame 2 hello arrives with
/// `role_type="observer"`.  Returns the Z85-encoded broker observer
/// pubkey the producer currently trusts (learned via REG_ACK), or empty
/// string if none is known (broker hasn't published one yet, or the
/// producer's stash hasn't been populated).  Empty return → observer
/// handshakes are rejected with a clear diagnostic.
///
/// The accessor is invoked ONCE per observer handshake — under
/// `RoleAPIBase::broker_observer_pubkey_z85()` in production, which
/// snapshots the value under a shared_mutex.  Thread-safe by design.
using ObserverPubkeyAccessor = std::function<std::string()>;

/// Exception type raised when the handshake exceeds its budget
/// (recv poll timeout or send poll timeout in `recv_all_until` /
/// `send_all`).  Task #300 (2026-07-03): callers of
/// `initiate_consumer_handshake` and `AttachProtocolAcceptor::
/// accept_one` catch this specifically to distinguish a startup-race
/// timeout (retry-worthy: producer L1 bound but L2 accept thread not
/// spawned yet, or vice-versa) from a genuine protocol error
/// (framing / JSON / crypto — not retry-worthy).  Prior to task #300,
/// timeout was a bare `std::runtime_error` indistinguishable from
/// protocol errors; the consumer-side retry loop caught only
/// ECONNREFUSED (nullopt path from `::connect`) and bailed on
/// timeout, dropping the remaining retry budget on the exact race
/// the loop was written to absorb.
class PYLABHUB_UTILS_EXPORT AttachProtocolTimeout : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// Material the consumer needs to assemble the hello and the
/// challenge-response.  Mirrors what the consumer's role obtains
/// from its KeyStore + the producer's pubkey from CONSUMER_REG_ACK
/// (HEP-CORE-0036 §6.4 `producers[i].zmq_pubkey` — same shape for
/// SHM channels under HEP-0041).
///
/// The consumer's own identity keypair is cited by KeyStore ENTRY
/// NAME (`own_seckey_name`).  SMS resolves it internally via
/// `secure().box_encrypt_using(name, ...)` — the raw seckey bytes
/// never appear at the AttachProtocol API boundary (HEP-CORE-0043
/// §1.4 use-not-export).  Standard values: `kRoleIdentityName` in
/// production; a test-local name after `seed_curve_identities()`
/// in tests.  `pubkey_z85` is the caller's own public half (Z85);
/// travels on the wire in Frame 2 for the producer to encrypt to.
struct ConsumerAuthMaterial
{
    std::string role_uid;
    std::string pubkey_z85;
    std::string own_seckey_name;
};

/// What the producer-side L2 returns after a successful accept +
/// hello + challenge-response verification.  The L2 caller (substep
/// 1e) then runs the broker pre-confirm (`CONSUMER_ATTACH_REQ_SHM`) and,
/// if confirmed, sends the capability fd via the L1
/// `send_capability`.
struct PYLABHUB_UTILS_EXPORT AuthenticatedConsumer
{
    IShmCapabilityProducer::AcceptedPeer raw_peer;
    std::string                          consumer_role_uid;
    std::string                          consumer_pubkey_z85;
};

#if defined(PYLABHUB_PLATFORM_LINUX)

/// Producer-side L2.  Wraps an `IShmCapabilityProducer` (L1) by
/// reference + the deployment's expected uid + a seckey accessor;
/// calls into L1 to accept peers and runs the challenge-response on
/// each.
class PYLABHUB_UTILS_EXPORT AttachProtocolAcceptor
{
public:
    /// @param transport             L1 IShmCapabilityProducer; must
    ///                              outlive this acceptor.
    /// @param expected_uid          Deployment's uid; SO_PEERCRED.uid
    ///                              must match or the peer is rejected.
    /// @param own_seckey_name       KeyStore entry name for the
    ///                              producer's identity keypair.  SMS
    ///                              resolves it internally via
    ///                              `secure().box_decrypt_using(name,
    ///                              ...)` and
    ///                              `secure().box_encrypt_using(name,
    ///                              ...)` (Frame-3 mutual-auth); the
    ///                              seckey bytes never cross the
    ///                              AttachProtocol API boundary
    ///                              (HEP-CORE-0043 §1.4 / §6).
    ///                              MUST be present in `secure().keys()`
    ///                              before `accept_one` is called.
    /// @param broker_observer_pubkey_accessor
    ///                              Callback returning the current
    ///                              known broker-observer pubkey Z85
    ///                              (dynamic, updated via REG_ACK).
    ///                              Optional.
    AttachProtocolAcceptor(IShmCapabilityProducer &transport,
                           uid_t                   expected_uid,
                           std::string             own_seckey_name,
                           ObserverPubkeyAccessor  broker_observer_pubkey_accessor = nullptr);

    ~AttachProtocolAcceptor() = default;

    AttachProtocolAcceptor(const AttachProtocolAcceptor &)            = delete;
    AttachProtocolAcceptor &operator=(const AttachProtocolAcceptor &) = delete;
    AttachProtocolAcceptor(AttachProtocolAcceptor &&)                 = delete;
    AttachProtocolAcceptor &operator=(AttachProtocolAcceptor &&)      = delete;

    /// Accept one consumer, run the challenge-response, return the
    /// authenticated peer + claimed identity.
    ///
    /// Returns `nullopt` on transport timeout (no consumer connected
    /// within `timeout`).  Throws `std::runtime_error` on:
    ///   - SO_PEERCRED uid mismatch.
    ///   - hello framing / size limit / JSON parse / field-shape
    ///     validation failure.
    ///   - challenge-response MAC verification failure (cryptographic
    ///     proof failed — caller almost certainly should NOT log the
    ///     pubkey at WARN because an attacker could spam this).
    ///   - any libsodium / kernel call failing.
    ///
    /// On any throw, the underlying peer socket fd (whose lifetime
    /// is normally caller-owned per `AcceptedPeer` semantics) is
    /// CLOSED here so the caller does not have to chase fd leaks
    /// across exception boundaries.
    std::optional<AuthenticatedConsumer>
    accept_one(std::chrono::milliseconds timeout);

private:
    IShmCapabilityProducer &transport_;
    uid_t                   expected_uid_;
    std::string             own_seckey_name_;
    ObserverPubkeyAccessor  broker_observer_pubkey_accessor_;
};

/// Consumer-side counterpart.  Connect to producer's endpoint, run
/// the consumer half of the challenge-response, and return the
/// connected fd ready for substep 1f's SCM_RIGHTS `recvmsg`.  The
/// returned fd is **caller-owned**.
///
/// Returns `nullopt` on connect-failure when the endpoint is simply
/// not present (`ECONNREFUSED` / `ENOENT`) — this is the normal
/// startup-race shape and not an error.
///
/// Throws `std::runtime_error` on protocol-level failures (framing,
/// JSON, size limit, libsodium failure, peer disconnect mid-frame).
///
/// **HEP-CORE-0041 §D4.5 mutual auth (task #262, opt-in 2026-07-03).**
/// When `require_mutual_auth == true`, the consumer:
///   1. Sends `consumer_nonce_b64` + `consumer_challenge_b64` as extra
///      fields on the Frame 2 hello.
///   2. After Frame 2, waits for producer's Frame 3
///      `{producer_pubkey_z85, proof_response_b64}`.
///   3. Verifies `producer_pubkey_z85` matches `producer_pubkey_z85`
///      argument (broker-supplied expectation).
///   4. Runs `crypto_box_open_easy` on `proof_response_b64` and
///      requires the plaintext to equal the challenge sent in step 1.
///   5. Any missing / mismatched piece raises `std::runtime_error`
///      with an `attach_producer_not_authenticated` marker.
/// When `require_mutual_auth == false` (default), the consumer keeps
/// the original 2-frame flow (backward compatible with pre-#262 producers).
PYLABHUB_UTILS_EXPORT std::optional<int>
initiate_consumer_handshake(const std::string          &endpoint,
                            const ConsumerAuthMaterial &self,
                            const std::string          &producer_pubkey_z85,
                            std::chrono::milliseconds   timeout,
                            bool require_mutual_auth = false);

#endif // PYLABHUB_PLATFORM_LINUX

// NOTE — Phase 4b + Phase 4c declarations that follow are conceptually
// portable (they use only IAttachChannel + SMS + JSON) but are
// currently declared inside the same `PYLABHUB_PLATFORM_LINUX` guard
// because their DEFINITIONS in `attach_protocol.cpp` /
// `attach_protocol_zmq.cpp` live alongside the SHM-specific code
// and share the anonymous-namespace utility helpers (b64_encode /
// z85_pubkey_to_raw / constants).  When non-Linux SHM backends land
// (tasks #259 / #260 / #261), the correct fix is to hoist the
// utility helpers + Phase 4b/4c out of the platform guard in the
// .cpp — then this outer guard can be dropped.  Gating now keeps
// declaration and definition in lockstep and avoids the "declared
// on all platforms, defined only on Linux" linker error.
#if defined(PYLABHUB_PLATFORM_LINUX)

// ============================================================================
//   Transport-agnostic handshake helpers (Phase 4b — 2026-07-07)
// ============================================================================
//
// These free functions run the AttachProtocol challenge-response over
// ANY conforming `IAttachChannel`.  The caller owns the transport
// (SHM fd, ZMQ ROUTER, future backends), performs the transport-layer
// peer identity check (SO_PEERCRED uid for SHM, ZAP-verified pubkey
// for ZMQ), wraps the transport in a channel, and calls the helper.
// The channel abstracts framing; these helpers own the crypto flow.
//
// SEPARATION OF CONCERNS:
//   - Transport-specific (SHM AttachProtocolAcceptor / ZMQ acceptor):
//     accept + peer identity check + channel construction + on-success
//     completion (SCM_RIGHTS handoff for SHM, channel_url response for
//     ZMQ).
//   - Transport-agnostic (these helpers): Frame 1/2/3 challenge-
//     response over IAttachChannel, using SMS for all crypto.

/// Result of a successful producer-side handshake — everything the
/// transport-specific acceptor needs to complete its post-handshake
/// step (fd handoff or channel_url response).
struct ProducerHandshakeResult
{
    /// Consumer's role UID as declared in Frame 2.
    std::string consumer_role_uid;
    /// Consumer's Z85-encoded CURVE pubkey as declared in Frame 2
    /// (verified: consumer proved possession of the matching seckey
    /// via the challenge-response).
    std::string consumer_pubkey_z85;
    /// `"consumer"` (default / pre-#317) or `"observer"` (HEP-CORE-0041
    /// §D1(d) broker-observer path).  Never empty in the returned
    /// result — the helper canonicalizes empty/absent to `"consumer"`.
    std::string role_type;
};

/// Run the producer-side AttachProtocol challenge-response over
/// `channel`.  Transport-agnostic — the caller has already accepted
/// the peer and verified its transport-layer identity (SO_PEERCRED
/// uid for SHM, ZAP-verified CURVE pubkey for ZMQ).
///
/// @param channel             Per-peer channel already bound to the
///                            accepted peer.
/// @param own_seckey_name     KeyStore entry name for the producer's
///                            own seckey (SMS reads it internally via
///                            `keys().with_seckey`).
/// @param observer_pubkey_accessor
///                            Callable returning the broker's
///                            observer pubkey Z85.  Empty return
///                            OR nullptr callable rejects any Frame 2
///                            with `role_type == "observer"`.  See
///                            HEP-CORE-0041 §D1(d) task #317 C.2.a.
/// @param deadline            Absolute deadline for the entire
///                            handshake (Frame 1 send + Frame 2 recv
///                            + optional Frame 3 send).
///
/// @return  Verified handshake result on success.
/// @throws  AttachProtocolTimeout  on any deadline expiry.
/// @throws  std::runtime_error     on protocol violation, wrong
///                                 pubkey, MAC verify failure, etc.
[[nodiscard]] PYLABHUB_UTILS_EXPORT ProducerHandshakeResult
run_producer_handshake(IAttachChannel                       &channel,
                       const std::string                    &own_seckey_name,
                       const ObserverPubkeyAccessor         &observer_pubkey_accessor,
                       std::chrono::steady_clock::time_point deadline);

/// Run the consumer-side AttachProtocol challenge-response over
/// `channel`.  Transport-agnostic — the caller has already connected
/// to the endpoint and wrapped the connection in a channel.
///
/// @param channel               Per-peer channel already bound to the
///                              producer.
/// @param self                  Consumer's own auth material (role UID,
///                              own pubkey Z85, KeyStore name for own
///                              seckey).
/// @param producer_pubkey_z85   Producer's expected Z85 pubkey (from
///                              broker's CONSUMER_REG_ACK).
/// @param deadline              Absolute deadline for the whole
///                              handshake.
/// @param require_mutual_auth   If true, consumer sends Frame 2 with
///                              its own nonce+challenge and requires
///                              a matching Frame 3 producer proof.
///
/// @throws AttachProtocolTimeout  on Frame 1 recv timeout OR Frame 2
///                                send timeout — signals H3a race
///                                (caller should retry the whole
///                                connect+handshake).  Frame 3 recv
///                                timeout maps to `std::runtime_error`
///                                below.
/// @throws std::runtime_error     on protocol violation, cipher
///                                verification failure, Frame 3
///                                timeout (a producer that spoke
///                                Frames 1+2 then went quiet is
///                                affirmatively refusing to
///                                authenticate — no retry).
PYLABHUB_UTILS_EXPORT void
run_consumer_handshake(IAttachChannel                       &channel,
                       const ConsumerAuthMaterial           &self,
                       const std::string                    &producer_pubkey_z85,
                       std::chrono::steady_clock::time_point deadline,
                       bool                                  require_mutual_auth);

} // namespace pylabhub::utils::security

// ============================================================================
//   ZMQ AttachProtocol acceptor + consumer entry (Phase 4c minimal —
//   2026-07-07)
// ============================================================================
//
// Wraps the `IAttachChannel` seam with a ZMQ ROUTER/DEALER transport
// (parallel to `AttachProtocolAcceptor` + `initiate_consumer_handshake`
// on the SHM side).  These entries compose:
//   ZmqAttachChannel  +  run_{producer,consumer}_handshake
//
// # Intended integration (Phase 4c-cont, deferred)
//
// These entries are the building blocks for BrokerService's future
// belt-and-braces AttachProtocol layer on top of CURVE.  In that
// design:
//   - Broker's REG_REQ handler captures the peer's routing identity
//     from the incoming ROUTER frame.
//   - Handler pauses REG_REQ processing, invokes
//     `ZmqAttachProtocolAcceptor::run_handshake(router, peer_routing_id, timeout)`,
//     receives a verified `ProducerHandshakeResult`.
//   - Only after successful verification does the broker send
//     REG_ACK and mark the peer `Authorized`.
//   - Role side runs `initiate_zmq_consumer_handshake` on its DEALER
//     immediately after sending REG_REQ, before treating any
//     REG_ACK as authoritative.
//
// **Current status: not yet wired.**  Phase 4c minimal ships the
// building blocks + L2 tests demonstrating the ZMQ transport works
// end-to-end; BrokerService integration is Phase 4c-cont (tracked in
// `docs/todo/AUTH_TODO.md`).  That integration requires reorganizing
// the broker's single-turn REG_REQ handler into a per-peer state
// machine so handshake frames from peer A can interleave with normal
// messages from peer B on the shared ROUTER — a structural refactor
// larger than adding these entries.

namespace pylabhub::utils::security
{

/// ZMQ AttachProtocol acceptor — composes `ZmqAttachChannel` +
/// `run_producer_handshake`.  Transport-symmetric parallel to
/// `AttachProtocolAcceptor` (SHM).  The caller provides an already-
/// bound ROUTER socket + the peer's routing identity (usually
/// captured from an earlier ROUTER recv).
///
/// ⚠  SECURITY REQUIREMENT — belt-and-braces layering only.
/// ------------------------------------------------------------------
/// This acceptor is designed to run AFTER libzmq's ZAP has already
/// pinned the peer's routing identity to an allowlisted CURVE
/// pubkey.  AttachProtocol on its own only proves "peer holds *some*
/// seckey matching *some* pubkey they claimed" — an attacker can
/// mint a fresh keypair, claim its pubkey, and pass every crypto
/// check here.  Without a prior CURVE + ZAP allowlist pin at the
/// TRANSPORT layer, this acceptor accepts any self-consistent
/// identity and is NOT a security boundary.
///
/// Caller invariants:
///   1. The ROUTER socket is configured with `ZMQ_CURVE_SERVER = 1`
///      (or an equivalent authenticated transport).
///   2. A ZAP handler has already validated the peer's CURVE public
///      key against the operator's allowlist BEFORE the peer's
///      routing identity reached the caller.
///   3. The `peer_routing_id` passed to `run_handshake` came from
///      a ROUTER recv on THAT authenticated socket — never from
///      user input or an unauthenticated source.
///
/// Violate any of the above and this acceptor provides no
/// meaningful authentication.  Phase 4c-cont's BrokerService
/// wiring will encode this as a compile-time / runtime check;
/// this Phase 4c minimal doesn't yet enforce it programmatically.
/// ------------------------------------------------------------------
class PYLABHUB_UTILS_EXPORT ZmqAttachProtocolAcceptor
{
public:
    /// @param own_seckey_name           KeyStore entry name for
    ///                                  producer's own seckey.
    /// @param observer_pubkey_accessor  Optional accessor for the
    ///                                  broker's observer pubkey
    ///                                  (empty = observer path
    ///                                  rejected).
    ZmqAttachProtocolAcceptor(std::string             own_seckey_name,
                              ObserverPubkeyAccessor  observer_pubkey_accessor);

    ~ZmqAttachProtocolAcceptor() = default;

    ZmqAttachProtocolAcceptor(const ZmqAttachProtocolAcceptor &)            = delete;
    ZmqAttachProtocolAcceptor &operator=(const ZmqAttachProtocolAcceptor &) = delete;
    ZmqAttachProtocolAcceptor(ZmqAttachProtocolAcceptor &&)                 = delete;
    ZmqAttachProtocolAcceptor &operator=(ZmqAttachProtocolAcceptor &&)      = delete;

    /// Run the producer-side AttachProtocol handshake with a specific
    /// peer already identified on the ROUTER.
    ///
    /// @param router            Already-bound ZMQ ROUTER socket.
    /// @param peer_routing_id   Peer's routing identity (from prior
    ///                          ROUTER recv metadata).
    /// @param timeout           Wall-clock budget for the entire
    ///                          handshake (Frame 1 send + Frame 2
    ///                          recv + optional Frame 3 send).
    ///
    /// @return  Verified handshake result — role_uid, pubkey_z85,
    ///          role_type.
    /// @throws  AttachProtocolTimeout  on deadline expiry.
    /// @throws  std::runtime_error     on protocol violation, crypto
    ///                                 verification failure, etc.
    [[nodiscard]] ProducerHandshakeResult
    run_handshake(zmq::socket_t             &router,
                  const std::string         &peer_routing_id,
                  std::chrono::milliseconds  timeout);

private:
    std::string             own_seckey_name_;
    ObserverPubkeyAccessor  observer_pubkey_accessor_;
};

/// Consumer-side entry for a ZMQ AttachProtocol handshake.  Assumes
/// `dealer` is already connected to the broker's ROUTER.  Runs the
/// same crypto flow as `initiate_consumer_handshake` (SHM) via the
/// `IAttachChannel` seam.
///
/// @param dealer               ZMQ DEALER socket, already connected.
/// @param self                 Consumer's own auth material.
/// @param producer_pubkey_z85  Producer's expected Z85 pubkey.
/// @param timeout              Wall-clock budget for the handshake.
/// @param require_mutual_auth  Opt-in for Frame 3 producer proof.
///
/// @throws AttachProtocolTimeout  on Frame 1 recv OR Frame 2 send
///                                timeout — signals the H3a race
///                                (caller may retry).
/// @throws std::runtime_error     on protocol violation, Frame 3
///                                timeout, crypto verification
///                                failure, etc.
PYLABHUB_UTILS_EXPORT void
initiate_zmq_consumer_handshake(
    zmq::socket_t              &dealer,
    const ConsumerAuthMaterial &self,
    const std::string          &producer_pubkey_z85,
    std::chrono::milliseconds   timeout,
    bool                        require_mutual_auth = false);

} // namespace pylabhub::utils::security

#endif // PYLABHUB_PLATFORM_LINUX — see the NOTE above line 262
