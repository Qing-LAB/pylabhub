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
 * **Use-not-export discipline** (HEP-CORE-0040 §5.2): callers give
 * the AttachProtocol access to their seckey via a callback that
 * scopes the seckey span to the callback body.  Bytes never leave
 * the security module's view; production wires this to
 * `KeyStore::with_seckey(...)`; tests inject a test-local seckey
 * directly without dragging KeyStore lifecycle into Pattern 1 tests.
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

/// Use-not-export accessor for a CURVE seckey.
///
/// The owner passes a callback that the AttachProtocol invokes with
/// the seckey bytes (32 raw bytes from Curve25519) within a scoped
/// lifetime.  Production wires this to
/// `KeyStore::with_seckey(kRoleIdentityName, ...)`; tests can wire a
/// local seckey directly.
///
/// **Lifecycle.**  Caller invokes `accessor(use_seckey)`; the
/// accessor looks up its seckey and calls `use_seckey(span_over_32_bytes)`;
/// `use_seckey` does the crypto and returns; the accessor may then
/// wipe and release.  After `accessor` returns, the seckey span is
/// no longer valid.
using SeckeyAccessor =
    std::function<void(std::function<void(std::span<const std::byte>)>)>;

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
struct ConsumerAuthMaterial
{
    std::string    role_uid;
    std::string    pubkey_z85;
    SeckeyAccessor seckey_accessor;
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
    /// @param producer_seckey_accessor
    ///                              Scoped access to the producer's
    ///                              seckey for `crypto_box_open_easy`.
    ///                              Must be non-empty.
    AttachProtocolAcceptor(IShmCapabilityProducer &transport,
                           uid_t                   expected_uid,
                           SeckeyAccessor          producer_seckey_accessor,
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
    SeckeyAccessor          producer_seckey_accessor_;
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

} // namespace pylabhub::utils::security
