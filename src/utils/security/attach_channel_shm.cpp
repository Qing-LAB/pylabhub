/**
 * @file attach_channel_shm.cpp
 * @brief SHM/AF_UNIX implementation of `IAttachChannel`.  See
 *        `attach_channel_shm.hpp` for the wire format + deadline
 *        contract.  Previously anonymous helpers inside
 *        `attach_protocol.cpp` (send_all/recv_all_until/send_frame/
 *        receive_frame); moved here 2026-07-07 as part of the
 *        Phase 3 transport-abstraction split so ZMQ AttachChannel
 *        can reuse `AttachProtocolAcceptor`'s protocol layer without
 *        entanglement.
 */
#include "utils/security/attach_channel_shm.hpp"

#include "utils/security/attach_protocol.hpp"  // AttachProtocolTimeout

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(PYLABHUB_PLATFORM_LINUX)
#    include <poll.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

#if defined(PYLABHUB_PLATFORM_LINUX)

namespace pylabhub::utils::security
{

namespace
{

// Deadline + errno helpers moved to `attach_channel.hpp` (Priority 3 /
// B2/B3 consolidation, 2026-07-07) — this file uses
// `attach_remaining_ms` / `attach_make_errno_error` directly.

void
recv_all_until(int fd, void *buf, std::size_t n,
               std::chrono::steady_clock::time_point deadline,
               const char *side)
{
    auto       *p   = static_cast<std::byte *>(buf);
    std::size_t got = 0;
    while (got < n)
    {
        const auto rem = attach_remaining_ms(deadline);
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
            if (e == EINTR) continue;
            throw attach_make_errno_error(side, "poll failed", e);
        }
        const ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r == 0)
        {
            throw std::runtime_error(
                std::string{"AttachProtocol::"} + side +
                ": peer closed connection mid-frame (got " +
                std::to_string(got) + " of " + std::to_string(n) + ")");
        }
        if (r < 0)
        {
            const int e = errno;
            if (e == EINTR) continue;
            throw attach_make_errno_error(side, "recv failed", e);
        }
        got += static_cast<std::size_t>(r);
    }
}

void
send_all(int fd, const void *buf, std::size_t n,
         std::chrono::steady_clock::time_point deadline,
         const char *side)
{
    const auto *p    = static_cast<const std::byte *>(buf);
    std::size_t sent = 0;
    while (sent < n)
    {
        const auto rem = attach_remaining_ms(deadline);
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
            if (e == EINTR) continue;
            throw attach_make_errno_error(side, "send poll failed", e);
        }
        const ssize_t r = ::send(fd, p + sent, n - sent,
                                  MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r < 0)
        {
            const int e = errno;
            if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK) continue;
            throw attach_make_errno_error(side, "send failed", e);
        }
        sent += static_cast<std::size_t>(r);
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────
// ShmAttachChannel
// ─────────────────────────────────────────────────────────────────

void
ShmAttachChannel::send_frame(const nlohmann::json                  &frame,
                              std::chrono::steady_clock::time_point  deadline)
{
    const std::string s = frame.dump();
    if (s.size() > IAttachChannel::kMaxAttachFrameBytes)
    {
        throw std::runtime_error(
            std::string{"AttachProtocol::"} + side_ +
            ": frame too large (" + std::to_string(s.size()) + " > " +
            std::to_string(IAttachChannel::kMaxAttachFrameBytes) + ")");
    }
    const auto         len   = static_cast<std::uint32_t>(s.size());
    const std::uint8_t lb[4] = {
        static_cast<std::uint8_t>(len & 0xFF),
        static_cast<std::uint8_t>((len >> 8)  & 0xFF),
        static_cast<std::uint8_t>((len >> 16) & 0xFF),
        static_cast<std::uint8_t>((len >> 24) & 0xFF),
    };
    send_all(fd_, lb, 4, deadline, side_.c_str());
    send_all(fd_, s.data(), s.size(), deadline, side_.c_str());
}

nlohmann::json
ShmAttachChannel::recv_frame(std::chrono::steady_clock::time_point deadline)
{
    std::uint8_t lb[4]{};
    recv_all_until(fd_, lb, 4, deadline, side_.c_str());
    const std::uint32_t len = static_cast<std::uint32_t>(lb[0]) |
                              (static_cast<std::uint32_t>(lb[1]) << 8) |
                              (static_cast<std::uint32_t>(lb[2]) << 16) |
                              (static_cast<std::uint32_t>(lb[3]) << 24);
    if (len == 0)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side_ +
                                 ": frame length is zero");
    }
    if (len > IAttachChannel::kMaxAttachFrameBytes)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side_ +
                                 ": frame length " + std::to_string(len) +
                                 " exceeds DoS cap " +
                                 std::to_string(IAttachChannel::kMaxAttachFrameBytes));
    }
    std::vector<char> body(len);
    recv_all_until(fd_, body.data(), len, deadline, side_.c_str());
    try
    {
        return nlohmann::json::parse(body.begin(), body.end());
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error(std::string{"AttachProtocol::"} + side_ +
                                 ": JSON parse failed: " + e.what());
    }
}

} // namespace pylabhub::utils::security

#endif // PYLABHUB_PLATFORM_LINUX
