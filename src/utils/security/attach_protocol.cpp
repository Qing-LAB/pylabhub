/**
 * @file attach_protocol.cpp
 * @brief Linux implementation of the producer-side L2 attach protocol
 *        for HEP-CORE-0041 SHM channels (substep 1c).
 *
 * See attach_protocol.hpp for the full design + protocol flow.  This
 * file owns the JSON length-prefixed framing and the per-call error
 * closure (close peer fd before any throw so callers don't leak).
 *
 * All cryptographic PROTOCOL operations route through SMS
 * (HEP-CORE-0043 §6):
 *   - Challenge-response ciphers: `secure().box_encrypt_using(name, ...)`
 *     / `secure().box_decrypt_using(name, ...)` — name-based seckey
 *     citation, bytes never cross the API boundary.
 *   - Nonces + challenge: `secure().random_bytes(...)`.
 *   - Constant-time compare: `secure().memcmp_ct(...)`.
 *   - Scrub: `secure().memzero(...)`.
 *
 * Base64 encode/decode of the JSON payload (not a crypto primitive,
 * just a text-safe binary encoding) still uses libsodium's
 * `sodium_bin2base64` / `sodium_base642bin` directly.  Since
 * `attach_protocol.cpp` lives INSIDE `src/utils/security/`, this is
 * legal per HEP-CORE-0043 §1.2 mechanism 4 (sodium.h stays inside
 * the security-module directory).  Base64 helpers may migrate to
 * SMS as Category 1a in a follow-up if other callers ever need them.
 */
#include "utils/security/attach_protocol.hpp"
#include "utils/security/attach_channel_shm.hpp"  // Phase 3 transport-abstraction seam
#include "utils/security/key_store.hpp"  // needed for secure().keys().{pubkey, with_seckey}

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sodium.h>  // Base64 helpers only — see file docstring.

#if defined(PYLABHUB_PLATFORM_LINUX)
// <poll.h> removed 2026-07-07 (Priority 3/B4) — all polling is inside
// `attach_channel_shm.cpp` after the Phase 3 framing extraction.
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>

extern "C"
{
// libzmq's Z85 decoder.  Converts a 40-char Z85 string into 32 raw
// bytes; returns dest on success, NULL on bad alphabet/length.  Used
// here for consistency with the rest of the codebase
// (curve_keypair.cpp comment cites zmq_z85_decode as the canonical
// path for CURVE keys).
uint8_t *zmq_z85_decode(uint8_t *dest, const char *src);
} // extern "C"
#endif

namespace pylabhub::utils::security
{

#if defined(PYLABHUB_PLATFORM_LINUX)

namespace
{

// kMaxFrameBytes moved to `IAttachChannel::kMaxAttachFrameBytes`
// (Priority 3/B1 consolidation, 2026-07-07) — this file no longer
// framesbytes and no longer needs the constant.
constexpr std::size_t kZ85PubkeyChars  = 40;
constexpr std::size_t kRawKeyBytes     = SecureSubsystem::kBoxPubkeyBytes;   // 32
constexpr std::size_t kNonceBytes      = SecureSubsystem::kBoxNonceBytes;    // 24
constexpr std::size_t kChallengeBytes  = 16;
constexpr std::size_t kMacBytes        = SecureSubsystem::kBoxMacBytes;      // 16
constexpr const char *kProtocolVersion = "hep-0041-1";

// `make_errno_error` lives in `attach_channel.hpp` as
// `attach_make_errno_error` (Priority 3/B3 consolidation) — this file
// uses it directly for AF_UNIX socket()/connect()/close() error paths.
//
// Framing (length-prefixed JSON) + deadline discipline lives in
// `ShmAttachChannel` (attach_channel_shm.cpp) since Phase 3 —
// `AttachProtocolAcceptor` here wraps the fd in a channel and
// delegates.  See `attach_channel.hpp` for the transport-abstraction
// seam.

std::string
b64_encode(std::span<const unsigned char> bin)
{
    const auto enc_max =
        sodium_base64_encoded_len(bin.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string out(enc_max, '\0');
    if (sodium_bin2base64(out.data(), out.size(), bin.data(), bin.size(),
                          sodium_base64_VARIANT_ORIGINAL) == nullptr)
    {
        throw std::runtime_error("AttachProtocol: b64 encode failed");
    }
    // sodium_bin2base64 null-terminates inside `enc_max`; trim to the
    // actual character count.
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::vector<unsigned char>
b64_decode(const std::string &s)
{
    if (s.empty())
    {
        throw std::runtime_error("AttachProtocol: b64 decode of empty string");
    }
    std::vector<unsigned char> out(s.size());
    std::size_t                bin_len = 0;
    if (sodium_base642bin(out.data(), out.size(), s.data(), s.size(),
                          nullptr, &bin_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0)
    {
        throw std::runtime_error("AttachProtocol: b64 decode failed");
    }
    out.resize(bin_len);
    return out;
}

std::array<unsigned char, kRawKeyBytes>
z85_pubkey_to_raw(const std::string &z85)
{
    if (z85.size() != kZ85PubkeyChars)
    {
        throw std::runtime_error("AttachProtocol: pubkey_z85 must be " +
                                 std::to_string(kZ85PubkeyChars) +
                                 " chars (got " + std::to_string(z85.size()) +
                                 ")");
    }
    std::array<unsigned char, kRawKeyBytes> raw{};
    if (::zmq_z85_decode(raw.data(), z85.c_str()) == nullptr)
    {
        throw std::runtime_error("AttachProtocol: Z85 decode failed for "
                                 "pubkey_z85 (invalid alphabet/length)");
    }
    return raw;
}

// AttachProtocol used to call sodium_init here as a workaround for the
// SMS module never doing its job.  Removed 2026-07-04 — the gate now
// lives inside SMS itself.  Callers who reach AttachProtocol without
// SMS being up hit `crypto_box_*` UB — but that's the caller's problem.

} // anonymous namespace

// ============================================================================
//   AttachProtocolAcceptor
// ============================================================================

AttachProtocolAcceptor::AttachProtocolAcceptor(
    IShmCapabilityProducer &transport, uid_t expected_uid,
    std::string            own_seckey_name,
    ObserverPubkeyAccessor broker_observer_pubkey_accessor)
    : transport_(transport),
      expected_uid_(expected_uid),
      own_seckey_name_(std::move(own_seckey_name)),
      broker_observer_pubkey_accessor_(std::move(broker_observer_pubkey_accessor))
{
    if (own_seckey_name_.empty())
    {
        throw std::invalid_argument(
            "AttachProtocolAcceptor: own_seckey_name must be non-empty");
    }
}

std::optional<AuthenticatedConsumer>
AttachProtocolAcceptor::accept_one(std::chrono::milliseconds timeout)
{
    auto peer = transport_.accept_one(timeout);
    if (!peer.has_value())
        return std::nullopt;

    // From here, this method OWNS the peer fd until either success
    // (returned in AuthenticatedConsumer.raw_peer for the caller to
    // close after capability send) or failure (we close before
    // throwing so the caller never sees a leaked fd through an
    // exception boundary).
    const int fd = peer->peer_socket_fd;
    if (fd < 0)
    {
        throw std::runtime_error(
            "AttachProtocolAcceptor: transport returned invalid peer fd");
    }

    // RAII guard: closes fd on scope exit unless `release()` is called.
    struct FdGuard
    {
        int  fd_;
        bool released{false};
        ~FdGuard()
        {
            if (!released && fd_ >= 0)
                ::close(fd_);
        }
        void release() { released = true; }
    } guard{fd};

    // #318 + #319 (2026-07-02): compute a SHARED deadline for the
    // entire handshake (send frame 1 + recv frame 2 + all sub-recvs).
    // Prior version passed the same `timeout` value to each recv/send
    // call — worst-case wall time was ~3× the documented budget (send
    // + recv-len + recv-body).  Sharing the deadline bounds the whole
    // step at exactly `timeout`.
    const auto handshake_deadline =
        std::chrono::steady_clock::now() + timeout;

    // ── 1. SO_PEERCRED uid sanity (HEP-CORE-0036 §I8) ────────────────────
    // Transport-layer peer identity check.  This is SHM-specific
    // (POSIX-uid semantics on AF_UNIX + SO_PEERCRED); a future ZMQ
    // AttachProtocol acceptor would substitute a ZAP-verified CURVE
    // pubkey check here.  Everything AFTER this point is
    // transport-agnostic and flows through `IAttachChannel`.
    if (peer->uid != expected_uid_)
    {
        throw std::runtime_error(
            "AttachProtocol::producer: SO_PEERCRED uid mismatch (expected " +
            std::to_string(expected_uid_) + ", got " +
            std::to_string(peer->uid) + ")");
    }

    // Wrap the fd in a channel — subsequent send/recv route through
    // the transport-agnostic seam (Phase 3).  Channel is non-owning;
    // `guard` above retains fd close responsibility.
    ShmAttachChannel channel(fd, "producer");

    // Phase 4b delegate — the actual Frame 1/2/3 crypto flow lives in
    // `run_producer_handshake`, a transport-agnostic helper.  This
    // method's ONLY remaining SHM-specific concerns are:
    //   (a) the transport accept + SO_PEERCRED uid check above,
    //   (b) the SCM_RIGHTS fd handoff below (guard.release()).
    const ProducerHandshakeResult phr = run_producer_handshake(
        channel, own_seckey_name_,
        broker_observer_pubkey_accessor_,
        handshake_deadline);

    // ── 9. Success.  Release the fd to the caller. ──────────────────────
    guard.release();
    AuthenticatedConsumer ac;
    ac.raw_peer            = *peer;
    ac.consumer_role_uid   = phr.consumer_role_uid;
    ac.consumer_pubkey_z85 = phr.consumer_pubkey_z85;
    return ac;
}


// ============================================================================
//   Consumer-side handshake
// ============================================================================

std::optional<int>
initiate_consumer_handshake(const std::string          &endpoint,
                            const ConsumerAuthMaterial &self,
                            const std::string          &producer_pubkey_z85,
                            std::chrono::milliseconds   timeout,
                            bool                        require_mutual_auth)
{

    // Boundary validation — throw on programmer error, return nullopt
    // only for "endpoint not present right now" which is a normal
    // startup race.
    //
    // §5.1 endpoint carries the canonical `unix://...` URI form on the
    // wire (CONSUMER_REG_ACK.shm_capability_endpoint is delivered
    // verbatim from `default_shm_capability_endpoint`).  The bare
    // filesystem path is what AF_UNIX expects in `sun_path` — strip the
    // scheme once here, before BOTH the size check and the memcpy
    // below, so the validation and the connect target agree on the
    // same string.  Cross-platform: every backend that lands here uses
    // an AF_UNIX-style filesystem path; non-Linux backends will gain
    // their own path-normalization shim.
    sockaddr_un       addr{};
    const std::string sock_path = strip_unix_scheme(endpoint);
    if (sock_path.empty())
    {
        throw std::invalid_argument(
            "initiate_consumer_handshake: endpoint must be non-empty");
    }
    if (sock_path.size() >= sizeof(addr.sun_path))
    {
        throw std::invalid_argument(
            "initiate_consumer_handshake: endpoint path too long");
    }
    if (self.role_uid.empty())
    {
        throw std::invalid_argument(
            "initiate_consumer_handshake: self.role_uid must be non-empty");
    }
    if (self.pubkey_z85.size() != kZ85PubkeyChars)
    {
        throw std::invalid_argument(
            "initiate_consumer_handshake: self.pubkey_z85 must be " +
            std::to_string(kZ85PubkeyChars) + " chars");
    }
    if (producer_pubkey_z85.size() != kZ85PubkeyChars)
    {
        throw std::invalid_argument(
            "initiate_consumer_handshake: producer_pubkey_z85 must be " +
            std::to_string(kZ85PubkeyChars) + " chars");
    }
    if (self.own_seckey_name.empty())
    {
        throw std::invalid_argument(
            "initiate_consumer_handshake: self.own_seckey_name must be non-empty");
    }

    // ── 1. Connect ───────────────────────────────────────────────────────
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1)
    {
        throw attach_make_errno_error("consumer", "socket failed", errno);
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size());
    addr.sun_path[sock_path.size()] = '\0';
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) ==
        -1)
    {
        const int e = errno;
        ::close(fd);
        if (e == ECONNREFUSED || e == ENOENT)
        {
            // Endpoint not present yet — normal startup race; caller
            // can poll back.
            return std::nullopt;
        }
        throw attach_make_errno_error(
            "consumer", ("connect to '" + endpoint + "' failed").c_str(), e);
    }

    // RAII guard: closes fd on any exception path.
    struct FdGuard
    {
        int  fd_;
        bool released{false};
        ~FdGuard()
        {
            if (!released && fd_ >= 0)
                ::close(fd_);
        }
        void release() { released = true; }
    } guard{fd};

    // #318 + #319 (2026-07-02): shared handshake deadline (see
    // AttachProtocolAcceptor::accept_one for full rationale).
    const auto handshake_deadline =
        std::chrono::steady_clock::now() + timeout;

    // Wrap the fd in a channel — Phase 3 transport-abstraction seam.
    // Channel is non-owning; `guard` retains fd close responsibility.
    ShmAttachChannel channel(fd, "consumer");

    // Phase 4b delegate — Frame 1/2/3 crypto flow lives in the
    // transport-agnostic helper.  On Frame 1 recv or Frame 2 send
    // timeout (H3a race), the helper throws `AttachProtocolTimeout`;
    // convert to nullopt so callers retry the connect+handshake.
    try
    {
        run_consumer_handshake(channel, self, producer_pubkey_z85,
                               handshake_deadline, require_mutual_auth);
    }
    catch (const AttachProtocolTimeout &)
    {
        // #300 (2026-07-03): pre-Frame-3 timeouts are the H3a race
        // class — producer bound the socket but the L2 accept thread
        // isn't ready yet.  Return nullopt so the caller retries.
        // Frame 3 timeout is converted to std::runtime_error inside
        // `run_consumer_handshake`, so this catch only sees the
        // retryable path.
        return std::nullopt;
    }

    // ── 6. Success.  Hand fd to caller. ─────────────────────────────────
    guard.release();
    return fd;
}

// ============================================================================
//   Transport-agnostic handshake helpers (Phase 4b — 2026-07-07)
// ============================================================================
//
// See header for the design rationale and public contract.  These two
// helpers own the Frame 1/2/3 cryptographic flow; the transport
// wrapper (`AttachProtocolAcceptor::accept_one` / `initiate_consumer_handshake`
// / future `ZmqAttachProtocolAcceptor`) owns accept + peer identity +
// post-handshake completion.  Both helpers use ONLY the `IAttachChannel`
// abstraction for I/O — no direct fd, socket, or ZMQ operations.

ProducerHandshakeResult
run_producer_handshake(IAttachChannel                       &channel,
                       const std::string                    &own_seckey_name,
                       const ObserverPubkeyAccessor         &observer_pubkey_accessor,
                       std::chrono::steady_clock::time_point deadline)
{
    if (own_seckey_name.empty())
    {
        throw std::invalid_argument(
            "run_producer_handshake: own_seckey_name must be non-empty");
    }

    // ── 2. Generate nonce + challenge ────────────────────────────────────
    unsigned char nonce[kNonceBytes]{};
    unsigned char challenge[kChallengeBytes]{};
    secure().random_bytes(nonce,     kNonceBytes);
    secure().random_bytes(challenge, kChallengeBytes);

    // ── 3. Send frame 1 ──────────────────────────────────────────────────
    {
        nlohmann::json out;
        out["protocol_version"] = kProtocolVersion;
        out["nonce_b64"]        = b64_encode({nonce, kNonceBytes});
        out["challenge_b64"]    = b64_encode({challenge, kChallengeBytes});
        channel.send_frame(out, deadline);
    }

    // ── 4. Receive frame 2 ───────────────────────────────────────────────
    const nlohmann::json hello = channel.recv_frame(deadline);

    // ── 5. Validate hello shape ──────────────────────────────────────────
    if (hello.value("protocol_version", std::string{}) != kProtocolVersion)
    {
        throw std::runtime_error(
            "AttachProtocol::producer: hello protocol_version mismatch");
    }
    const std::string role_uid = hello.value("role_uid", std::string{});
    if (role_uid.empty())
    {
        throw std::runtime_error("AttachProtocol::producer: hello role_uid empty");
    }
    // HEP-CORE-0041 §10.5 observer-role wiring (#317 C.1, 2026-07-03).
    // See original in-place documentation (pre-Phase-4b) for the full
    // route-on-role_type rationale.  Canonicalize empty/absent to
    // "consumer" so callers see a non-empty `role_type` in the result.
    std::string role_type =
        hello.value("role_type", std::string{"consumer"});
    if (role_type.empty()) role_type = "consumer";
    if (role_type != "consumer" && role_type != "observer")
    {
        throw std::runtime_error(
            "AttachProtocol::producer: hello role_type must be "
            "\"consumer\", \"observer\", or empty (got \"" + role_type + "\")");
    }
    const std::string pubkey_z85 = hello.value("pubkey_z85", std::string{});
    if (pubkey_z85.size() != kZ85PubkeyChars)
    {
        throw std::runtime_error(
            "AttachProtocol::producer: hello pubkey_z85 must be " +
            std::to_string(kZ85PubkeyChars) + " chars (got " +
            std::to_string(pubkey_z85.size()) + ")");
    }
    if (role_type == "observer")
    {
        // HEP-CORE-0041 §D1(d) observer handshake identity check.
        if (!observer_pubkey_accessor)
        {
            throw std::runtime_error(
                "AttachProtocol::producer: role_type=\"observer\" received "
                "but no observer pubkey accessor installed (caller must "
                "pass one — HEP-CORE-0041 §D1(d) task #317 C.2.b).");
        }
        const std::string trusted_broker_pubkey = observer_pubkey_accessor();
        if (trusted_broker_pubkey.empty())
        {
            throw std::runtime_error(
                "AttachProtocol::producer: role_type=\"observer\" received "
                "but no broker observer pubkey known yet (broker did not "
                "publish `broker_observer_pubkey_z85` on REG_ACK, or "
                "REG_ACK has not been processed yet — HEP-CORE-0041 "
                "§D1(d) task #317 C.2.a).");
        }
        if (pubkey_z85.size() != trusted_broker_pubkey.size() ||
            !secure().memcmp_ct(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t *>(pubkey_z85.data()),
                    pubkey_z85.size()),
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t *>(trusted_broker_pubkey.data()),
                    trusted_broker_pubkey.size())))
        {
            throw std::runtime_error(
                "AttachProtocol::producer: role_type=\"observer\" hello "
                "pubkey_z85 does not match the broker observer pubkey "
                "the producer trusts (rejecting; possible squatter or "
                "broker-restart with unlearned new keypair).");
        }
    }
    const std::string response_b64 =
        hello.value("challenge_response_b64", std::string{});
    if (response_b64.empty())
    {
        throw std::runtime_error(
            "AttachProtocol::producer: hello challenge_response_b64 missing");
    }

    // ── 6. Decode pubkey + cipher ────────────────────────────────────────
    const auto consumer_pk_raw = z85_pubkey_to_raw(pubkey_z85);
    const auto cipher          = b64_decode(response_b64);
    if (cipher.size() != kChallengeBytes + kMacBytes)
    {
        throw std::runtime_error(
            "AttachProtocol::producer: challenge_response size mismatch "
            "(expected " +
            std::to_string(kChallengeBytes + kMacBytes) + " bytes, got " +
            std::to_string(cipher.size()) + ")");
    }

    // ── 7. Verify challenge under producer's seckey (SMS Cat 1c) ─────────
    unsigned char plaintext[kChallengeBytes]{};
    const std::size_t decoded = secure().box_decrypt_using(
        own_seckey_name,
        std::span<const std::uint8_t, SecureSubsystem::kBoxPubkeyBytes>(
            reinterpret_cast<const std::uint8_t *>(consumer_pk_raw.data()),
            SecureSubsystem::kBoxPubkeyBytes),
        std::span<const std::uint8_t, SecureSubsystem::kBoxNonceBytes>(
            nonce, SecureSubsystem::kBoxNonceBytes),
        std::span<const std::uint8_t>(cipher.data(), cipher.size()),
        std::span<std::uint8_t>(plaintext, kChallengeBytes));
    const bool verified =
        (decoded == kChallengeBytes) &&
        secure().memcmp_ct(
            std::span<const std::uint8_t>(plaintext, kChallengeBytes),
            std::span<const std::uint8_t>(challenge, kChallengeBytes));
    secure().memzero(std::span<std::uint8_t>(plaintext, kChallengeBytes));

    if (!verified)
    {
        throw std::runtime_error(
            "AttachProtocol::producer: challenge-response verification "
            "failed (consumer does not hold the seckey corresponding to "
            "the claimed pubkey)");
    }

    // ── 8. Optional mutual-auth Frame 3 (HEP-CORE-0041 §D4.5, #262) ──────
    {
        std::string cons_nonce_b64;
        std::string cons_challenge_b64;
        try
        {
            cons_nonce_b64 =
                hello.value("consumer_nonce_b64", std::string{});
            cons_challenge_b64 =
                hello.value("consumer_challenge_b64", std::string{});
        }
        catch (const nlohmann::json::exception &e)
        {
            throw std::runtime_error(
                std::string("AttachProtocol::producer: consumer mutual-auth "
                            "field wrong type — ") + e.what());
        }
        if (!cons_nonce_b64.empty() || !cons_challenge_b64.empty())
        {
            if (cons_nonce_b64.empty() || cons_challenge_b64.empty())
            {
                throw std::runtime_error(
                    "AttachProtocol::producer: consumer sent partial "
                    "mutual-auth fields (consumer_nonce_b64 and "
                    "consumer_challenge_b64 must both be present or both absent)");
            }
            const auto cons_nonce_vec     = b64_decode(cons_nonce_b64);
            const auto cons_challenge_vec = b64_decode(cons_challenge_b64);
            if (cons_nonce_vec.size() != kNonceBytes ||
                cons_challenge_vec.size() != kChallengeBytes)
            {
                throw std::runtime_error(
                    "AttachProtocol::producer: consumer mutual-auth field size "
                    "mismatch (expected nonce=" +
                    std::to_string(kNonceBytes) + ", challenge=" +
                    std::to_string(kChallengeBytes) + ")");
            }

            std::vector<unsigned char> proof_cipher(kChallengeBytes + kMacBytes);
            const std::size_t proof_written = secure().box_encrypt_using(
                own_seckey_name,
                std::span<const std::uint8_t, SecureSubsystem::kBoxPubkeyBytes>(
                    reinterpret_cast<const std::uint8_t *>(consumer_pk_raw.data()),
                    SecureSubsystem::kBoxPubkeyBytes),
                std::span<const std::uint8_t, SecureSubsystem::kBoxNonceBytes>(
                    cons_nonce_vec.data(), SecureSubsystem::kBoxNonceBytes),
                std::span<const std::uint8_t>(
                    cons_challenge_vec.data(), kChallengeBytes),
                std::span<std::uint8_t>(
                    proof_cipher.data(), proof_cipher.size()));
            if (proof_written != proof_cipher.size())
            {
                throw std::runtime_error(
                    "AttachProtocol::producer: box_encrypt_using for "
                    "Frame 3 proof failed (secret " + own_seckey_name +
                    " missing from KeyStore, or sodium error)");
            }

            const std::string_view producer_pk_z85 =
                secure().keys().pubkey(own_seckey_name);
            nlohmann::json proof;
            proof["producer_pubkey_z85"] = std::string(producer_pk_z85);
            proof["proof_response_b64"] =
                b64_encode({proof_cipher.data(), proof_cipher.size()});
            channel.send_frame(proof, deadline);
        }
    }

    ProducerHandshakeResult result;
    result.consumer_role_uid   = role_uid;
    result.consumer_pubkey_z85 = pubkey_z85;
    result.role_type           = std::move(role_type);
    return result;
}

void
run_consumer_handshake(IAttachChannel                       &channel,
                       const ConsumerAuthMaterial           &self,
                       const std::string                    &producer_pubkey_z85,
                       std::chrono::steady_clock::time_point deadline,
                       bool                                  require_mutual_auth)
{
    if (self.role_uid.empty())
    {
        throw std::invalid_argument(
            "run_consumer_handshake: self.role_uid must be non-empty");
    }
    if (self.pubkey_z85.size() != kZ85PubkeyChars)
    {
        throw std::invalid_argument(
            "run_consumer_handshake: self.pubkey_z85 must be " +
            std::to_string(kZ85PubkeyChars) + " chars");
    }
    if (producer_pubkey_z85.size() != kZ85PubkeyChars)
    {
        throw std::invalid_argument(
            "run_consumer_handshake: producer_pubkey_z85 must be " +
            std::to_string(kZ85PubkeyChars) + " chars");
    }
    if (self.own_seckey_name.empty())
    {
        throw std::invalid_argument(
            "run_consumer_handshake: self.own_seckey_name must be non-empty");
    }

    // ── 2. Receive + parse frame 1 ───────────────────────────────────────
    // Frame 1 recv timeout AND Frame 2 send timeout bubble up as
    // AttachProtocolTimeout — the caller decides whether to convert
    // to a nullopt-retry (SHM H3a race) or rethrow.
    const nlohmann::json challenge_in = channel.recv_frame(deadline);
    // Priority 6/D1 (2026-07-07): wrap `.value()` in try/catch so a
    // peer sending `{"protocol_version": 42, ...}` (wrong-typed
    // field) surfaces as an AttachProtocol-branded error rather than
    // an uncaught nlohmann `type_error.302`.  Symmetric with the
    // Frame 3 wrap below.
    std::string protocol_version;
    std::string nonce_b64_field;
    std::string challenge_b64_field;
    try
    {
        protocol_version =
            challenge_in.value("protocol_version", std::string{});
        nonce_b64_field =
            challenge_in.value("nonce_b64", std::string{});
        challenge_b64_field =
            challenge_in.value("challenge_b64", std::string{});
    }
    catch (const nlohmann::json::exception &e)
    {
        throw std::runtime_error(
            std::string("AttachProtocol::consumer: Frame 1 field wrong "
                        "type — ") + e.what());
    }
    if (protocol_version != kProtocolVersion)
    {
        throw std::runtime_error(
            "AttachProtocol::consumer: challenge protocol_version mismatch");
    }
    const auto nonce_dec = b64_decode(nonce_b64_field);
    if (nonce_dec.size() != kNonceBytes)
    {
        throw std::runtime_error(
            "AttachProtocol::consumer: nonce size mismatch (expected " +
            std::to_string(kNonceBytes) + " bytes, got " +
            std::to_string(nonce_dec.size()) + ")");
    }
    const auto challenge_dec = b64_decode(challenge_b64_field);
    if (challenge_dec.size() != kChallengeBytes)
    {
        throw std::runtime_error(
            "AttachProtocol::consumer: challenge size mismatch (expected " +
            std::to_string(kChallengeBytes) + " bytes, got " +
            std::to_string(challenge_dec.size()) + ")");
    }

    std::array<unsigned char, kNonceBytes>     nonce{};
    std::array<unsigned char, kChallengeBytes> challenge{};
    std::memcpy(nonce.data(),     nonce_dec.data(),     kNonceBytes);
    std::memcpy(challenge.data(), challenge_dec.data(), kChallengeBytes);

    // ── 3. Encrypt response under consumer's seckey (SMS Cat 1c) ─────────
    const auto producer_pk_raw = z85_pubkey_to_raw(producer_pubkey_z85);
    std::vector<unsigned char> cipher(kChallengeBytes + kMacBytes);
    const std::size_t written = secure().box_encrypt_using(
        self.own_seckey_name,
        std::span<const std::uint8_t, SecureSubsystem::kBoxPubkeyBytes>(
            reinterpret_cast<const std::uint8_t *>(producer_pk_raw.data()),
            SecureSubsystem::kBoxPubkeyBytes),
        std::span<const std::uint8_t, SecureSubsystem::kBoxNonceBytes>(
            nonce.data(), SecureSubsystem::kBoxNonceBytes),
        std::span<const std::uint8_t>(challenge.data(), kChallengeBytes),
        std::span<std::uint8_t>(cipher.data(), cipher.size()));
    if (written != cipher.size())
    {
        throw std::runtime_error(
            "AttachProtocol::consumer: box_encrypt_using failed (secret '" +
            self.own_seckey_name + "' missing from KeyStore, or sodium error)");
    }

    // ── 4. Build + send frame 2 ──────────────────────────────────────────
    nlohmann::json hello;
    hello["protocol_version"]      = kProtocolVersion;
    hello["role_uid"]               = self.role_uid;
    hello["pubkey_z85"]             = self.pubkey_z85;
    hello["challenge_response_b64"] = b64_encode({cipher.data(), cipher.size()});
    hello["role_type"]              = "consumer";

    // HEP-CORE-0041 §D4.5 mutual-auth extension (#262, opt-in 2026-07-03).
    std::array<unsigned char, kNonceBytes>     consumer_nonce{};
    std::array<unsigned char, kChallengeBytes> consumer_challenge{};
    if (require_mutual_auth)
    {
        secure().random_bytes(consumer_nonce.data(),     kNonceBytes);
        secure().random_bytes(consumer_challenge.data(), kChallengeBytes);
        hello["consumer_nonce_b64"] =
            b64_encode({consumer_nonce.data(), kNonceBytes});
        hello["consumer_challenge_b64"] =
            b64_encode({consumer_challenge.data(), kChallengeBytes});
    }

    channel.send_frame(hello, deadline);

    // ── 5. Mutual-auth Frame 3 (opt-in per §D4.5) ────────────────────────
    if (require_mutual_auth)
    {
        nlohmann::json proof;
        try
        {
            proof = channel.recv_frame(deadline);
        }
        catch (const AttachProtocolTimeout &)
        {
            // Frame 3 missing — treat as producer-not-authenticated
            // rather than nullopt-retry.  A producer that opened the
            // socket, spoke Frames 1+2, and then went quiet is
            // affirmatively failing to prove identity.
            throw std::runtime_error(
                "AttachProtocol::consumer: producer did not send Frame 3 within budget "
                "(attach_producer_not_authenticated) — either the producer is a "
                "pre-#262 build that does not support mutual auth, or the peer "
                "is not the real producer");
        }
        std::string proof_producer_pk;
        std::string proof_response_b64;
        try
        {
            proof_producer_pk =
                proof.value("producer_pubkey_z85", std::string{});
            proof_response_b64 =
                proof.value("proof_response_b64", std::string{});
        }
        catch (const nlohmann::json::exception &e)
        {
            throw std::runtime_error(
                std::string("AttachProtocol::consumer: Frame 3 field wrong "
                            "type (attach_producer_not_authenticated) — ") +
                e.what());
        }
        if (proof_producer_pk.size() != kZ85PubkeyChars ||
            proof_response_b64.empty())
        {
            throw std::runtime_error(
                "AttachProtocol::consumer: Frame 3 malformed "
                "(attach_producer_not_authenticated) — expected "
                "producer_pubkey_z85 + proof_response_b64");
        }
        if (proof_producer_pk != producer_pubkey_z85)
        {
            throw std::runtime_error(
                "AttachProtocol::consumer: producer_pubkey_z85 mismatch "
                "(attach_producer_not_authenticated) — expected '" +
                producer_pubkey_z85 + "' but peer identified as '" +
                proof_producer_pk + "'");
        }
        const auto proof_cipher = b64_decode(proof_response_b64);
        if (proof_cipher.size() != kChallengeBytes + kMacBytes)
        {
            throw std::runtime_error(
                "AttachProtocol::consumer: proof_response_b64 size mismatch "
                "(attach_producer_not_authenticated) — expected " +
                std::to_string(kChallengeBytes + kMacBytes) + " bytes, got " +
                std::to_string(proof_cipher.size()));
        }
        std::array<unsigned char, kChallengeBytes> proof_plain{};
        const std::size_t proof_decoded = secure().box_decrypt_using(
            self.own_seckey_name,
            std::span<const std::uint8_t, SecureSubsystem::kBoxPubkeyBytes>(
                reinterpret_cast<const std::uint8_t *>(producer_pk_raw.data()),
                SecureSubsystem::kBoxPubkeyBytes),
            std::span<const std::uint8_t, SecureSubsystem::kBoxNonceBytes>(
                consumer_nonce.data(), SecureSubsystem::kBoxNonceBytes),
            std::span<const std::uint8_t>(
                proof_cipher.data(), proof_cipher.size()),
            std::span<std::uint8_t>(proof_plain.data(), kChallengeBytes));
        const bool proof_verified =
            (proof_decoded == kChallengeBytes) &&
            secure().memcmp_ct(
                std::span<const std::uint8_t>(
                    proof_plain.data(), kChallengeBytes),
                std::span<const std::uint8_t>(
                    consumer_challenge.data(), kChallengeBytes));
        secure().memzero(
            std::span<std::uint8_t>(proof_plain.data(), kChallengeBytes));
        if (!proof_verified)
        {
            throw std::runtime_error(
                "AttachProtocol::consumer: Frame 3 proof verification "
                "failed (attach_producer_not_authenticated) — producer "
                "does not hold the seckey corresponding to the pubkey "
                "the broker told us to expect");
        }
    }
}

#else // !PYLABHUB_PLATFORM_LINUX

#    error "HEP-CORE-0041 AttachProtocol: only Linux is implemented in substep 1c. FreeBSD/macOS/Windows L1 backends are #error'd (tasks #259/#260/#261); the L2 here would also need AcceptedPeer-variant branching when those land. Refusing to compile until implemented for this platform."

#endif // PYLABHUB_PLATFORM_LINUX

} // namespace pylabhub::utils::security
