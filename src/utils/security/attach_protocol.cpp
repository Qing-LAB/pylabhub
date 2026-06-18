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
void
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
            throw std::runtime_error(std::string{"AttachProtocol::"} + side +
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

void
send_all(int fd, const void *buf, std::size_t n, const char *side)
{
    const auto *p    = static_cast<const std::byte *>(buf);
    std::size_t sent = 0;
    while (sent < n)
    {
        const ssize_t r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0)
        {
            const int e = errno;
            if (e == EINTR)
                continue;
            throw make_errno_error(side, "send failed", e);
        }
        sent += static_cast<std::size_t>(r);
    }
}

void
send_frame(int fd, const nlohmann::json &body, const char *side)
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
    send_all(fd, lb, 4, side);
    send_all(fd, s.data(), s.size(), side);
}

nlohmann::json
receive_frame(int fd, std::chrono::milliseconds timeout, const char *side)
{
    std::uint8_t lb[4]{};
    recv_all(fd, lb, 4, timeout, side);
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
    recv_all(fd, body.data(), len, timeout, side);
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
        send_frame(fd, out, "producer");
    }

    // ── 4. Receive frame 2 ───────────────────────────────────────────────
    const nlohmann::json hello = receive_frame(fd, timeout, "producer");

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

    // ── 8. Success.  Release the fd to the caller. ──────────────────────
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
                            std::chrono::milliseconds   timeout)
{
    ensure_sodium_init();

    // Boundary validation — throw on programmer error, return nullopt
    // only for "endpoint not present right now" which is a normal
    // startup race.
    sockaddr_un addr{};
    if (endpoint.empty())
    {
        throw std::invalid_argument(
            "initiate_consumer_handshake: endpoint must be non-empty");
    }
    if (endpoint.size() >= sizeof(addr.sun_path))
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
    std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size());
    addr.sun_path[endpoint.size()] = '\0';
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

    // ── 2. Receive + parse frame 1 ───────────────────────────────────────
    const nlohmann::json challenge_in = receive_frame(fd, timeout, "consumer");
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
    send_frame(fd, hello, "consumer");

    // ── 5. Success.  Hand fd to caller. ─────────────────────────────────
    guard.release();
    return fd;
}

#else // !PYLABHUB_PLATFORM_LINUX

#    error "HEP-CORE-0041 AttachProtocol: only Linux is implemented in substep 1c. FreeBSD/macOS/Windows L1 backends are #error'd (tasks #259/#260/#261); the L2 here would also need AcceptedPeer-variant branching when those land. Refusing to compile until implemented for this platform."

#endif // PYLABHUB_PLATFORM_LINUX

} // namespace pylabhub::utils::security
