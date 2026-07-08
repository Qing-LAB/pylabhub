#pragma once
/**
 * @file attach_protocol.hpp
 * @brief AttachProtocol primitive + SHM binding — see HEP-CORE-0044.
 *
 * ## Authoritative design
 *
 * - **HEP-CORE-0044 (AttachProtocol)** — the primitive: wire spec,
 *   cryptographic contract, per-connection state machine, transport
 *   abstraction (`IAttachChannel`), transport-agnostic protocol
 *   helpers (`run_producer_handshake` / `run_consumer_handshake`),
 *   Frame 3 mutual auth (task #262), `role_type` extension.
 * - **HEP-CORE-0041 §5** — SHM channel authentication that uses this
 *   primitive as its Layer-2b handshake, plus the capability transport
 *   (memfd + SCM_RIGHTS) that runs after successful attach.
 * - **HEP-CORE-0045 §3** — Broker SHM observer path that uses this
 *   primitive with `role_type="observer"`.
 * - **HEP-CORE-0043 §6** — SMS asymmetric box surface
 *   (`box_encrypt_using` / `box_decrypt_using`) that AttachProtocol
 *   calls internally.
 * - **HEP-CORE-0040 §8.5.2** — raw-32-byte seckey representation at
 *   the security-module API boundary; AttachProtocol cites seckeys
 *   by KeyStore entry NAME (never raw bytes).
 *
 * ## Shipped API surface (this header)
 *
 * - `AttachProtocolAcceptor` — SHM binding wrapper.  Runs
 *   SO_PEERCRED uid check (HEP-CORE-0036 §I8), constructs a
 *   `ShmAttachChannel`, delegates to `run_producer_handshake`.
 * - `initiate_consumer_handshake` — SHM initiator wrapper.  Opens
 *   the Unix socket, constructs a `ShmAttachChannel`, delegates to
 *   `run_consumer_handshake`.
 * - `run_producer_handshake` / `run_consumer_handshake` — transport-
 *   agnostic protocol helpers taking an `IAttachChannel &`.  Reused
 *   by the SHM binding above and by HEP-CORE-0045's broker-observer
 *   client.
 * - `ProducerHandshakeResult`, `ConsumerAuthMaterial`,
 *   `AttachProtocolTimeout`, `ObserverPubkeyAccessor` — supporting
 *   types.
 *
 * ## Platform support
 *
 * Linux only for the SHM binding today.  Other platforms are
 * `#error`-guarded pending #259 / #260 / #261 (L1 SHM backends).
 * The transport-agnostic helpers (`run_*_handshake`) are portable —
 * they only depend on `IAttachChannel`, SMS, and JSON.
 */

#include "plh_platform.hpp"
#include "pylabhub_utils_export.h"
#include "utils/security/attach_channel.hpp"        // IAttachChannel
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

#endif // PYLABHUB_PLATFORM_LINUX — see the NOTE above line 262
