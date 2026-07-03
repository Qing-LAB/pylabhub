/**
 * @file attach_protocol.cpp
 * @brief Linux implementation of the producer-side L2 attach protocol
 *        for HEP-CORE-0041 SHM channels (substep 1c).
 *
 * See attach_protocol.hpp for the full design + protocol flow.  This
 * file owns the JSON length-prefixed framing, the libsodium
 * `crypto_box` challenge-response, and the per-call error closure
 * (close peer fd before any throw so callers don't leak).
 */
#include "utils/security/attach_protocol.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sodium.h>

#if defined(PYLABHUB_PLATFORM_LINUX)
#    include <poll.h>
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
// libzmq's Z85 encoder.  Converts 32 raw bytes into a 40-char Z85 string
// (plus null terminator).  Used only by the #262 mutual-auth Frame 3
// path to encode the pubkey the producer derives on the fly via
// `crypto_scalarmult_base`; the rest of the codebase gets pubkey_z85
// already-encoded from `KeyStore::add_identity` at startup.
char    *zmq_z85_encode(char *dest, const uint8_t *src, size_t size);
} // extern "C"
#endif

namespace pylabhub::utils::security
{

#if defined(PYLABHUB_PLATFORM_LINUX)

namespace
{

constexpr std::size_t kMaxFrameBytes   = 4096;
constexpr std::size_t kZ85PubkeyChars  = 40;
constexpr std::size_t kRawKeyBytes     = 32;
constexpr std::size_t kNonceBytes      = crypto_box_NONCEBYTES;   // 24
constexpr std::size_t kChallengeBytes  = 16;
constexpr std::size_t kMacBytes        = crypto_box_MACBYTES;     // 16
constexpr const char *kProtocolVersion = "hep-0041-1";

[[nodiscard]] std::runtime_error
make_errno_error(const char *side, const char *what, int captured_errno)
{
    return std::runtime_error(std::string{"AttachProtocol::"} + side + ": " +
                              what + ": " + std::strerror(captured_errno));
}

// Receive exactly `n` bytes from `fd` under a `total_timeout` budget,
// or throw.  Treats EOF (recv returns 0) as a transport error so a
// peer that drops mid-frame surfaces immediately rather than hanging.
[[maybe_unused]] void
recv_all(int fd, void *buf, std::size_t n,
         std::chrono::milliseconds total_timeout, const char *side)
{
    const auto deadline = std::chrono::steady_clock::now() + total_timeout;
    auto      *p        = static_cast<std::byte *>(buf);
    std::size_t got     = 0;
    while (got < n)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            throw std::runtime_error(std::string{"AttachProtocol::"} + side +
                                     ": timeout waiting for " +
                                     std::to_string(n) + " bytes (got " +
                                     std::to_string(got) + ")");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        pollfd pfd{fd, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (rc == 0)
        {
            throw AttachProtocolTimeout(std::string{"AttachProtocol::"} + side +
                                         ": poll timed out");
        }
        if (rc < 0)
        {
            const int e = errno;
            if (e == EINTR)
                continue;
            throw make_errno_error(side, "poll failed", e);
        }
        const ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r == 0)
        {
            throw std::runtime_error(std::string{"AttachProtocol::"} + side +
                                     ": peer closed connection mid-frame "
                                     "(got " + std::to_string(got) + " of " +
                                     std::to_string(n) + ")");
        }
        if (r < 0)
        {
            const int e = errno;
            if (e == EINTR)
                continue;
            throw make_errno_error(side, "recv failed", e);
        }
        got += static_cast<std::size_t>(r);
    }
}

// Compute remaining budget from an absolute deadline; zero if past.
// Used to bound each recv/send call against a SHARED deadline so an
// entire multi-call handshake step (recv len + recv body, or send len
// + send body) is bounded at exactly `timeout` — not `2 * timeout`.
[[nodiscard]] inline std::chrono::milliseconds
remaining_ms(std::chrono::steady_clock::time_point deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::chrono::milliseconds{0};
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

// recv_all variant that takes an absolute deadline directly.  Kept
// alongside the timeout-based overload for callers that don't need
// deadline sharing (unit tests, one-shot recv).
void
recv_all_until(int fd, void *buf, std::size_t n,
               std::chrono::steady_clock::time_point deadline,
               const char *side)
{
    auto      *p        = static_cast<std::byte *>(buf);
    std::size_t got     = 0;
    while (got < n)
    {
        const auto rem = remaining_ms(deadline);
        if (rem.count() == 0)
        {
            throw AttachProtocolTimeout(std::string{"AttachProtocol::"} + side +
                                         ": timeout waiting for " +
                                         std::to_string(n) + " bytes (got " +
                                         std::to_string(got) + ")");
        }
        pollfd pfd{fd, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(rem.count()));
        if (rc == 0)
        {
            throw AttachProtocolTimeout(std::string{"AttachProtocol::"} + side +
                                         ": poll timed out");
        }
        if (rc < 0)
        {
            const int e = errno;
            if (e == EINTR)
                continue;
            throw make_errno_error(side, "poll failed", e);
        }
        const ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r == 0)
        {
            throw std::runtime_error(std::string{"AttachProtocol::"} + side +
                                     ": peer closed connection mid-frame "
                                     "(got " + std::to_string(got) + " of " +
                                     std::to_string(n) + ")");
        }
        if (r < 0)
        {
            const int e = errno;
            if (e == EINTR)
                continue;
            throw make_errno_error(side, "recv failed", e);
        }
        got += static_cast<std::size_t>(r);
    }
}

// #319 (2026-07-02): send_all now takes an absolute deadline and
// polls POLLOUT with remaining budget before each ::send call.  Prior
// version had no deadline — a peer that connects then stops reading
// could stall the accept thread indefinitely, defeating
// ThreadManager::shutdown_requested() poll and blocking role teardown
// past the 5s stop-handler-threads quiescence budget.
void
send_all(int fd, const void *buf, std::size_t n,
         std::chrono::steady_clock::time_point deadline,
         const char *side)
{
    const auto *p    = static_cast<const std::byte *>(buf);
    std::size_t sent = 0;
    while (sent < n)
    {
        const auto rem = remaining_ms(deadline);
        if (rem.count() == 0)
        {
            throw AttachProtocolTimeout(
                std::string{"AttachProtocol::"} + side +
                ": send timeout (sent " + std::to_string(sent) + " of " +
                std::to_string(n) + " bytes) — peer socket buffer full "
                "or reader stalled");
        }
        pollfd pfd{fd, POLLOUT, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(rem.count()));
        if (rc == 0)
        {
            throw AttachProtocolTimeout(
                std::string{"AttachProtocol::"} + side +
                ": send poll timed out (sent " + std::to_string(sent) +
                " of " + std::to_string(n) + " bytes)");
        }
        if (rc < 0)
        {
            const int e = errno;
            if (e == EINTR)
                continue;
            throw make_errno_error(side, "send poll failed", e);
        }
        const ssize_t r = ::send(fd, p + sent, n - sent,
                                  MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r < 0)
        {
            const int e = errno;
            if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK)
                continue;
            throw make_errno_error(side, "send failed", e);
        }
        sent += static_cast<std::size_t>(r);
    }
}

void
send_frame(int fd, const nlohmann::json &body,
           std::chrono::steady_clock::time_point deadline,
           const char *side)
{
    const std::string s = body.dump();
    if (s.size() > kMaxFrameBytes)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side +
                                 ": frame too large (" +
                                 std::to_string(s.size()) + " > " +
                                 std::to_string(kMaxFrameBytes) + ")");
    }
    const auto len = static_cast<std::uint32_t>(s.size());
    const std::uint8_t lb[4] = {
        static_cast<std::uint8_t>(len & 0xFF),
        static_cast<std::uint8_t>((len >> 8) & 0xFF),
        static_cast<std::uint8_t>((len >> 16) & 0xFF),
        static_cast<std::uint8_t>((len >> 24) & 0xFF),
    };
    send_all(fd, lb, 4, deadline, side);
    send_all(fd, s.data(), s.size(), deadline, side);
}

// #318 (2026-07-02): receive_frame now takes an absolute deadline so
// the recv-length + recv-body pair is bounded at exactly `deadline`
// wall time.  Prior version passed the SAME `timeout` value to both
// recv_all calls — worst case handshake step burned 2× the documented
// budget (frame 1 lands right before deadline → frame 2 gets full
// timeout again).
nlohmann::json
receive_frame(int fd, std::chrono::steady_clock::time_point deadline,
              const char *side)
{
    std::uint8_t lb[4]{};
    recv_all_until(fd, lb, 4, deadline, side);
    const std::uint32_t len = static_cast<std::uint32_t>(lb[0]) |
                              (static_cast<std::uint32_t>(lb[1]) << 8) |
                              (static_cast<std::uint32_t>(lb[2]) << 16) |
                              (static_cast<std::uint32_t>(lb[3]) << 24);
    if (len == 0)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side +
                                 ": frame length is zero");
    }
    if (len > kMaxFrameBytes)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side +
                                 ": frame length " + std::to_string(len) +
                                 " exceeds DoS cap " +
                                 std::to_string(kMaxFrameBytes));
    }
    std::vector<char> body(len);
    recv_all_until(fd, body.data(), len, deadline, side);
    try
    {
        return nlohmann::json::parse(body.begin(), body.end());
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side +
                                 ": JSON parse failed: " + e.what());
    }
}

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

void
ensure_sodium_init()
{
    static const int rc = []() { return ::sodium_init(); }();
    if (rc < 0)
    {
        throw std::runtime_error("AttachProtocol: sodium_init failed");
    }
}

} // anonymous namespace

// ============================================================================
//   AttachProtocolAcceptor
// ============================================================================

AttachProtocolAcceptor::AttachProtocolAcceptor(
    IShmCapabilityProducer &transport, uid_t expected_uid,
    SeckeyAccessor producer_seckey_accessor)
    : transport_(transport),
      expected_uid_(expected_uid),
      producer_seckey_accessor_(std::move(producer_seckey_accessor))
{
    ensure_sodium_init();
    if (!producer_seckey_accessor_)
    {
        throw std::invalid_argument(
            "AttachProtocolAcceptor: producer_seckey_accessor must be set");
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
    if (peer->uid != expected_uid_)
    {
        throw std::runtime_error(
            "AttachProtocol::producer: SO_PEERCRED uid mismatch (expected " +
            std::to_string(expected_uid_) + ", got " +
            std::to_string(peer->uid) + ")");
    }

    // ── 2. Generate nonce + challenge ────────────────────────────────────
    unsigned char nonce[kNonceBytes]{};
    unsigned char challenge[kChallengeBytes]{};
    ::randombytes_buf(nonce, kNonceBytes);
    ::randombytes_buf(challenge, kChallengeBytes);

    // ── 3. Send frame 1 ──────────────────────────────────────────────────
    {
        nlohmann::json out;
        out["protocol_version"] = kProtocolVersion;
        out["nonce_b64"]        = b64_encode({nonce, kNonceBytes});
        out["challenge_b64"]    = b64_encode({challenge, kChallengeBytes});
        send_frame(fd, out, handshake_deadline, "producer");
    }

    // ── 4. Receive frame 2 ───────────────────────────────────────────────
    const nlohmann::json hello = receive_frame(fd, handshake_deadline,
                                                 "producer");

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
    // Route on `role_type`: `"consumer"` (or empty for pre-#317 role
    // builds) → existing CURVE-against-channel-allowlist path.
    // `"observer"` → broker-observer path (not yet implemented; reject
    // with clear diagnostic until #317 Phase C.2 broker-dial lands).
    // Any other value → protocol violation.
    const std::string role_type =
        hello.value("role_type", std::string{"consumer"});
    if (role_type != "consumer" && role_type != "observer")
    {
        throw std::runtime_error(
            "AttachProtocol::producer: hello role_type must be "
            "\"consumer\" or \"observer\" (got \"" + role_type + "\")");
    }
    if (role_type == "observer")
    {
        // Phase C.2 will add broker-observer verification.  Until it
        // does, an observer handshake is not yet supported; reject
        // cleanly so operators see a specific diagnostic rather than
        // a downstream CURVE-verify failure.
        throw std::runtime_error(
            "AttachProtocol::producer: role_type=\"observer\" received "
            "but broker-observer path not yet implemented (#317 Phase C.2 pending).");
    }
    const std::string pubkey_z85 = hello.value("pubkey_z85", std::string{});
    if (pubkey_z85.size() != kZ85PubkeyChars)
    {
        throw std::runtime_error(
            "AttachProtocol::producer: hello pubkey_z85 must be " +
            std::to_string(kZ85PubkeyChars) + " chars (got " +
            std::to_string(pubkey_z85.size()) + ")");
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

    // ── 7. Verify under producer's seckey ────────────────────────────────
    bool verified = false;
    producer_seckey_accessor_(
        [&](std::span<const std::byte> producer_sk_span) {
            if (producer_sk_span.size() != kRawKeyBytes)
            {
                throw std::runtime_error(
                    "AttachProtocol::producer: producer_sk span must be " +
                    std::to_string(kRawKeyBytes) + " bytes (got " +
                    std::to_string(producer_sk_span.size()) + ")");
            }
            unsigned char plaintext[kChallengeBytes]{};
            const int     rc = ::crypto_box_open_easy(
                plaintext, cipher.data(), cipher.size(), nonce,
                consumer_pk_raw.data(),
                reinterpret_cast<const unsigned char *>(producer_sk_span.data()));
            if (rc == 0 &&
                ::sodium_memcmp(plaintext, challenge, kChallengeBytes) == 0)
            {
                verified = true;
            }
            // Wipe local plaintext before leaving (it equals the
            // issued challenge, which isn't secret long-term, but
            // discipline matters).
            ::sodium_memzero(plaintext, kChallengeBytes);
        });

    if (!verified)
    {
        throw std::runtime_error(
            "AttachProtocol::producer: challenge-response verification "
            "failed (consumer does not hold the seckey corresponding to "
            "the claimed pubkey)");
    }

    // ── 8. Optional mutual-auth Frame 3 (HEP-CORE-0041 §D4.5, #262) ──────
    //
    // If the consumer sent `consumer_nonce_b64` + `consumer_challenge_b64`
    // on Frame 2, respond with Frame 3 proving possession of our seckey.
    // Absent fields → pre-#262 consumer that doesn't want mutual auth;
    // skip Frame 3 (backward compat).
    {
        // 2026-07-03 code review Finding #10 — catch json::type_error
        // for mutual-auth field type mismatches, symmetric with the
        // consumer-side Frame 3 wrap.  A peer sending
        // {"consumer_nonce_b64": null, ...} would otherwise throw a
        // bare nlohmann type_error that escapes past acceptor-side
        // callers.
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
            std::array<unsigned char, kRawKeyBytes> producer_pk_raw{};
            bool proof_ok = false;
            producer_seckey_accessor_(
                [&](std::span<const std::byte> producer_sk_span) {
                    if (producer_sk_span.size() != kRawKeyBytes)
                    {
                        throw std::runtime_error(
                            "AttachProtocol::producer: seckey span must be " +
                            std::to_string(kRawKeyBytes) + " bytes for Frame 3");
                    }
                    // Derive our pubkey from the seckey inside the accessor
                    // scope so we don't need to store or receive it separately.
                    // X25519: pubkey = base_point^seckey.  Same primitive
                    // crypto_box uses.
                    if (::crypto_scalarmult_base(
                            producer_pk_raw.data(),
                            reinterpret_cast<const unsigned char *>(producer_sk_span.data())) != 0)
                    {
                        throw std::runtime_error(
                            "AttachProtocol::producer: crypto_scalarmult_base "
                            "failed while deriving pubkey for Frame 3");
                    }
                    const int rc = ::crypto_box_easy(
                        proof_cipher.data(),
                        cons_challenge_vec.data(), kChallengeBytes,
                        cons_nonce_vec.data(), consumer_pk_raw.data(),
                        reinterpret_cast<const unsigned char *>(producer_sk_span.data()));
                    if (rc != 0)
                    {
                        throw std::runtime_error(
                            "AttachProtocol::producer: crypto_box_easy for "
                            "Frame 3 proof failed (rc=" +
                            std::to_string(rc) + ")");
                    }
                    proof_ok = true;
                });
            if (!proof_ok)
            {
                throw std::runtime_error(
                    "AttachProtocol::producer: seckey accessor did not run "
                    "for Frame 3 (programmer error)");
            }

            // Encode derived pubkey to z85 for the wire.
            char producer_pk_z85_buf[kZ85PubkeyChars + 1] = {};
            if (::zmq_z85_encode(producer_pk_z85_buf, producer_pk_raw.data(),
                                  kRawKeyBytes) == nullptr)
            {
                throw std::runtime_error(
                    "AttachProtocol::producer: zmq_z85_encode failed for "
                    "Frame 3 pubkey");
            }
            nlohmann::json proof;
            proof["producer_pubkey_z85"] = std::string(producer_pk_z85_buf);
            proof["proof_response_b64"] =
                b64_encode({proof_cipher.data(), proof_cipher.size()});
            send_frame(fd, proof, handshake_deadline, "producer");
        }
    }

    // ── 9. Success.  Release the fd to the caller. ──────────────────────
    guard.release();
    AuthenticatedConsumer ac;
    ac.raw_peer            = *peer;
    ac.consumer_role_uid   = role_uid;
    ac.consumer_pubkey_z85 = pubkey_z85;
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
    ensure_sodium_init();

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
    if (!self.seckey_accessor)
    {
        throw std::invalid_argument(
            "initiate_consumer_handshake: self.seckey_accessor must be set");
    }

    // ── 1. Connect ───────────────────────────────────────────────────────
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1)
    {
        throw make_errno_error("consumer", "socket failed", errno);
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
        throw make_errno_error(
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

    // ── 2. Receive + parse frame 1 ───────────────────────────────────────
    //
    // #300 (2026-07-03): if the receive TIMES OUT (producer bound the
    // Unix socket but the L2 accept thread hasn't spawned yet — the
    // exact H3a race the caller's retry loop was written to absorb),
    // return nullopt so the caller retries.  This mirrors the ENOENT /
    // ECONNREFUSED nullopt path above at the `connect` step.  A
    // protocol-level throw (framing / JSON / crypto — a plain
    // std::runtime_error) still propagates so callers bail on real
    // contract violations.
    nlohmann::json challenge_in;
    try
    {
        challenge_in = receive_frame(fd, handshake_deadline, "consumer");
    }
    catch (const AttachProtocolTimeout &)
    {
        return std::nullopt;
    }
    if (challenge_in.value("protocol_version", std::string{}) != kProtocolVersion)
    {
        throw std::runtime_error(
            "AttachProtocol::consumer: challenge protocol_version mismatch");
    }
    const auto nonce_dec =
        b64_decode(challenge_in.value("nonce_b64", std::string{}));
    if (nonce_dec.size() != kNonceBytes)
    {
        throw std::runtime_error(
            "AttachProtocol::consumer: nonce size mismatch (expected " +
            std::to_string(kNonceBytes) + " bytes, got " +
            std::to_string(nonce_dec.size()) + ")");
    }
    const auto challenge_dec =
        b64_decode(challenge_in.value("challenge_b64", std::string{}));
    if (challenge_dec.size() != kChallengeBytes)
    {
        throw std::runtime_error(
            "AttachProtocol::consumer: challenge size mismatch (expected " +
            std::to_string(kChallengeBytes) + " bytes, got " +
            std::to_string(challenge_dec.size()) + ")");
    }

    std::array<unsigned char, kNonceBytes>     nonce{};
    std::array<unsigned char, kChallengeBytes> challenge{};
    std::memcpy(nonce.data(), nonce_dec.data(), kNonceBytes);
    std::memcpy(challenge.data(), challenge_dec.data(), kChallengeBytes);

    // ── 3. Encrypt response under consumer's seckey ──────────────────────
    const auto producer_pk_raw = z85_pubkey_to_raw(producer_pubkey_z85);
    std::vector<unsigned char> cipher(kChallengeBytes + kMacBytes);
    bool                       encrypted = false;
    self.seckey_accessor([&](std::span<const std::byte> consumer_sk_span) {
        if (consumer_sk_span.size() != kRawKeyBytes)
        {
            throw std::runtime_error(
                "AttachProtocol::consumer: consumer_sk span must be " +
                std::to_string(kRawKeyBytes) + " bytes (got " +
                std::to_string(consumer_sk_span.size()) + ")");
        }
        const int rc = ::crypto_box_easy(
            cipher.data(), challenge.data(), kChallengeBytes, nonce.data(),
            producer_pk_raw.data(),
            reinterpret_cast<const unsigned char *>(consumer_sk_span.data()));
        if (rc != 0)
        {
            throw std::runtime_error(
                "AttachProtocol::consumer: crypto_box_easy failed (rc=" +
                std::to_string(rc) + ")");
        }
        encrypted = true;
    });
    if (!encrypted)
    {
        throw std::runtime_error(
            "AttachProtocol::consumer: seckey_accessor returned without "
            "invoking the callback (programmer error)");
    }

    // ── 4. Build + send frame 2 ──────────────────────────────────────────
    nlohmann::json hello;
    hello["protocol_version"]      = kProtocolVersion;
    hello["role_uid"]               = self.role_uid;
    hello["pubkey_z85"]             = self.pubkey_z85;
    hello["challenge_response_b64"] = b64_encode({cipher.data(), cipher.size()});
    // HEP-CORE-0041 §10.5 observer-role wiring (#317 C.1, 2026-07-03).
    // Explicit `role_type` field on the hello so producer's acceptor
    // can route consumer handshakes vs. broker-observer handshakes to
    // different verification paths.  Emitted as `"consumer"` here;
    // broker's observer-attach client will send `"observer"` (Phase C
    // when that path lands, still TODO).  Empty / absent field is
    // treated as `"consumer"` by the producer's acceptor for backward
    // compatibility with pre-#317 role builds.
    hello["role_type"]              = "consumer";

    // HEP-CORE-0041 §D4.5 mutual-auth extension (#262, opt-in 2026-07-03).
    // Consumer piggybacks its own nonce+challenge onto Frame 2 so the
    // producer can prove possession of its seckey in Frame 3.  Producer
    // that sees these fields responds with Frame 3; consumer that sent
    // them requires Frame 3.  Pre-#262 producers ignore the extras and
    // never send Frame 3 — which the consumer's requirement then flags
    // as `PRODUCER_NOT_AUTHENTICATED` (correct fail-loud behaviour when
    // the operator opted in to mutual auth against an outdated peer).
    std::array<unsigned char, kNonceBytes>     consumer_nonce{};
    std::array<unsigned char, kChallengeBytes> consumer_challenge{};
    if (require_mutual_auth)
    {
        ::randombytes_buf(consumer_nonce.data(), kNonceBytes);
        ::randombytes_buf(consumer_challenge.data(), kChallengeBytes);
        hello["consumer_nonce_b64"] =
            b64_encode({consumer_nonce.data(), kNonceBytes});
        hello["consumer_challenge_b64"] =
            b64_encode({consumer_challenge.data(), kChallengeBytes});
    }

    // #300 (2026-07-03): send timeout (producer's L2 accept thread
    // spawned but stalled reading) is same H3a-race class as the
    // recv timeout above — nullopt for retry, not a protocol error.
    try
    {
        send_frame(fd, hello, handshake_deadline, "consumer");
    }
    catch (const AttachProtocolTimeout &)
    {
        return std::nullopt;
    }

    // ── 5. Mutual-auth Frame 3 (opt-in per §D4.5) ────────────────────────
    if (require_mutual_auth)
    {
        nlohmann::json proof;
        try
        {
            proof = receive_frame(fd, handshake_deadline, "consumer");
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
        // 2026-07-03 code review Finding #7 — catch json::type_error
        // from `.value<string>()` when the field is present but
        // non-string.  Without this wrap, a peer sending
        // {"producer_pubkey_z85": 42, ...} triggers nlohmann's
        // `type_error.302` which escapes with the bare nlohmann
        // message, bypassing the intended
        // `attach_producer_not_authenticated` marker that §D4.5
        // callers grep for.
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
            // Peer identified with a different pubkey than the broker
            // told us to expect.  Squatter scenario per §D4.5 threat model.
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
        // Verify with our seckey against producer's expected pubkey.
        // Same use-not-export pattern as consumer's own signing above.
        bool proof_verified = false;
        self.seckey_accessor([&](std::span<const std::byte> consumer_sk_span) {
            if (consumer_sk_span.size() != kRawKeyBytes)
            {
                throw std::runtime_error(
                    "AttachProtocol::consumer: consumer_sk span must be " +
                    std::to_string(kRawKeyBytes) + " bytes for Frame 3 verify");
            }
            std::array<unsigned char, kChallengeBytes> proof_plain{};
            const int rc = ::crypto_box_open_easy(
                proof_plain.data(), proof_cipher.data(), proof_cipher.size(),
                consumer_nonce.data(), producer_pk_raw.data(),
                reinterpret_cast<const unsigned char *>(consumer_sk_span.data()));
            if (rc == 0 && ::sodium_memcmp(proof_plain.data(),
                                            consumer_challenge.data(),
                                            kChallengeBytes) == 0)
            {
                proof_verified = true;
            }
            ::sodium_memzero(proof_plain.data(), kChallengeBytes);
        });
        if (!proof_verified)
        {
            throw std::runtime_error(
                "AttachProtocol::consumer: Frame 3 crypto verify failed "
                "(attach_producer_not_authenticated) — producer does not hold "
                "the seckey corresponding to the expected pubkey");
        }
    }

    // ── 6. Success.  Hand fd to caller. ─────────────────────────────────
    guard.release();
    return fd;
}

#else // !PYLABHUB_PLATFORM_LINUX

#    error "HEP-CORE-0041 AttachProtocol: only Linux is implemented in substep 1c. FreeBSD/macOS/Windows L1 backends are #error'd (tasks #259/#260/#261); the L2 here would also need AcceptedPeer-variant branching when those land. Refusing to compile until implemented for this platform."

#endif // PYLABHUB_PLATFORM_LINUX

} // namespace pylabhub::utils::security
