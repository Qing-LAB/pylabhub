/**
 * @file shm_capability_channel.cpp
 * @brief Cross-platform SHM capability transport — Linux backend.
 *
 * Linux: anonymous SHM via `memfd_create`; producer binds an
 * AF_UNIX/SOCK_STREAM listener; consumer connects and receives the SHM
 * fd via `SCM_RIGHTS`; both sides `mmap` the same physical pages.  Uses
 * `SO_PEERCRED` + `struct ucred` for the SO_PEERCRED triple, which is a
 * Linux-only socket option (FreeBSD uses `LOCAL_PEERCRED` + `struct
 * xucred`; macOS likewise differs).  FreeBSD + macOS + Windows backends
 * ship later under HEP-0041 Phases 2-3 — on those platforms the
 * factories throw with a "no backend" message.
 *
 * @see docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md §5-§6, §13 Linux.
 */
#include "utils/security/shm_capability_channel.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#    include <fcntl.h>
#    include <poll.h>
#    include <sys/mman.h>
#    include <sys/socket.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <sys/un.h>
#    include <unistd.h>
#endif

namespace pylabhub::utils::security
{

#if defined(__linux__)

namespace
{

/// Build a runtime_error with a uniform "ShmCapability<Side>: <what>: <strerror>"
/// shape so the throw-point and the syscall failure are both surfaced for
/// debugging.  Keeping `errno` capture immediately at the call site is the
/// load-bearing discipline — later cleanup paths clobber errno.
[[nodiscard]] std::runtime_error
make_errno_error(const char *side, const char *what, int captured_errno)
{
    return std::runtime_error(std::string{"ShmCapability"} + side + ": " + what +
                              ": " + std::strerror(captured_errno));
}

/// Producer-side anonymous-SHM + Unix-socket listener.
class MemfdProducer final : public IShmCapabilityProducer
{
public:
    explicit MemfdProducer(size_t bytes);
    ~MemfdProducer() override;

    bool                        bind_endpoint(const std::string &endpoint) override;
    std::optional<AcceptedPeer> accept_one(std::chrono::milliseconds timeout) override;
    bool                        send_capability(int peer_socket_fd) override;
    std::span<std::byte>        data() override;
    [[nodiscard]] size_t        size() const noexcept override;

private:
    int         anon_fd_{-1};
    int         listen_fd_{-1};
    void       *mmap_base_{nullptr};
    size_t      mmap_size_{0};
    std::string bound_path_;
};

MemfdProducer::MemfdProducer(size_t bytes)
{
    if (bytes == 0)
    {
        throw std::invalid_argument(
            "ShmCapabilityProducer: size must be non-zero "
            "(HEP-CORE-0041 §6 — anonymous SHM regions are sized at construction).");
    }

    anon_fd_ = ::memfd_create("plh_shm_capability", MFD_CLOEXEC);
    if (anon_fd_ == -1)
    {
        throw make_errno_error("Producer", "memfd_create failed", errno);
    }

    if (::ftruncate(anon_fd_, static_cast<off_t>(bytes)) == -1)
    {
        const int captured = errno;
        ::close(anon_fd_);
        anon_fd_ = -1;
        throw make_errno_error("Producer", "ftruncate failed", captured);
    }

    void *base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                        anon_fd_, /*offset=*/0);
    if (base == MAP_FAILED)
    {
        const int captured = errno;
        ::close(anon_fd_);
        anon_fd_ = -1;
        throw make_errno_error("Producer", "mmap failed", captured);
    }

    mmap_base_ = base;
    mmap_size_ = bytes;
}

MemfdProducer::~MemfdProducer()
{
    if (mmap_base_ != nullptr)
    {
        ::munmap(mmap_base_, mmap_size_);
    }
    if (anon_fd_ != -1)
    {
        ::close(anon_fd_);
    }
    if (listen_fd_ != -1)
    {
        ::close(listen_fd_);
    }
    if (!bound_path_.empty())
    {
        ::unlink(bound_path_.c_str());
    }
}

bool
MemfdProducer::bind_endpoint(const std::string &endpoint)
{
    if (endpoint.empty())
    {
        return false;
    }
    if (listen_fd_ != -1)
    {
        // Already bound — interface contract: bind once per instance.
        return false;
    }
    // sun_path is a fixed-size array; reject too-long paths at the boundary.
    sockaddr_un addr{};
    if (endpoint.size() >= sizeof(addr.sun_path))
    {
        return false;
    }

    const int sock = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock == -1)
    {
        return false;
    }

    // Best-effort cleanup of a stale socket left by a prior crash.  The
    // recovery semantic is: a fresh process inheriting the same channel
    // identity should overwrite any leftover path, not refuse to start.
    ::unlink(endpoint.c_str());

    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size());
    addr.sun_path[endpoint.size()] = '\0';

    if (::bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == -1)
    {
        ::close(sock);
        return false;
    }

    // Tighten the path's permissions.  This is NOT the security gate —
    // L2 (HEP-CORE-0041 §9 D4 attach sequence, lands in task #250+) does
    // the cryptographic auth.  This is defence in depth so a path
    // accidentally created in a shared directory does not let an
    // arbitrary local UID even attempt the connect.
    ::chmod(endpoint.c_str(), 0700);

    if (::listen(sock, /*backlog=*/8) == -1)
    {
        ::close(sock);
        ::unlink(endpoint.c_str());
        return false;
    }

    listen_fd_  = sock;
    bound_path_ = endpoint;
    return true;
}

std::optional<IShmCapabilityProducer::AcceptedPeer>
MemfdProducer::accept_one(std::chrono::milliseconds timeout)
{
    if (listen_fd_ == -1)
    {
        throw std::runtime_error(
            "ShmCapabilityProducer::accept_one: not bound — "
            "call bind_endpoint() first.");
    }

    pollfd pfd{listen_fd_, POLLIN, 0};
    const int n = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (n == 0)
    {
        return std::nullopt;
    }
    if (n == -1)
    {
        const int captured = errno;
        if (captured == EINTR)
        {
            // Treat a signal as a timeout: the caller's outer loop polls back.
            // Surfacing EINTR as an exception would force every caller to
            // wrap accept_one() in a retry loop — the timeout return is the
            // right shape for "no peer this round."
            return std::nullopt;
        }
        throw make_errno_error("Producer::accept_one", "poll failed", captured);
    }

    const int peer = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (peer == -1)
    {
        throw make_errno_error("Producer::accept_one", "accept4 failed", errno);
    }

    AcceptedPeer result{};
    result.peer_socket_fd = peer;

    ucred           cred{};
    socklen_t       cred_len = sizeof(cred);
    if (::getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0)
    {
        result.pid = cred.pid;
        result.uid = cred.uid;
        result.gid = cred.gid;
    }
    // SO_PEERCRED failure leaves the fields at zero defaults.  The L2
    // auth layer (task #250) does an explicit equality check against the
    // expected uid; zero default fails closed for any non-root expectation.

    return result;
}

bool
MemfdProducer::send_capability(int peer_socket_fd)
{
    if (anon_fd_ == -1 || peer_socket_fd < 0)
    {
        return false;
    }

    // SCM_RIGHTS requires at least one byte of regular payload — a kernel
    // quirk; sendmsg() with zero iov bytes drops the ancillary data
    // silently on some kernels.  The byte itself is meaningless.
    char   iov_byte = 0;
    iovec  iov{&iov_byte, 1};
    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    union
    {
        char     buf[CMSG_SPACE(sizeof(int))];
        cmsghdr  align;
    } u{};
    std::memset(u.buf, 0, sizeof(u.buf));
    msg.msg_control    = u.buf;
    msg.msg_controllen = sizeof(u.buf);

    cmsghdr *cmsg    = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &anon_fd_, sizeof(int));

    const ssize_t sent = ::sendmsg(peer_socket_fd, &msg, MSG_NOSIGNAL);
    return sent == 1;
}

std::span<std::byte>
MemfdProducer::data()
{
    return {static_cast<std::byte *>(mmap_base_), mmap_size_};
}

size_t
MemfdProducer::size() const noexcept
{
    return mmap_size_;
}

/// Consumer-side: connect, receive SHM fd via SCM_RIGHTS, fstat the size,
/// mmap.  All set-up happens in the ctor; the socket is closed before the
/// ctor returns because we no longer need it (the kernel duped the fd
/// into our process during recvmsg).
class MemfdConsumer final : public IShmCapabilityConsumer
{
public:
    MemfdConsumer(const std::string &endpoint, std::chrono::milliseconds timeout);
    ~MemfdConsumer() override;

    std::span<std::byte> data() override;
    [[nodiscard]] size_t size() const noexcept override;

private:
    int    received_fd_{-1};
    void  *mmap_base_{nullptr};
    size_t mmap_size_{0};
};

MemfdConsumer::MemfdConsumer(const std::string       &endpoint,
                             std::chrono::milliseconds timeout)
{
    sockaddr_un addr{};
    if (endpoint.empty() || endpoint.size() >= sizeof(addr.sun_path))
    {
        throw std::invalid_argument(
            "ShmCapabilityConsumer: invalid endpoint "
            "(empty or longer than sockaddr_un::sun_path).");
    }

    const int sock = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock == -1)
    {
        throw make_errno_error("Consumer", "socket failed", errno);
    }

    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size());
    addr.sun_path[endpoint.size()] = '\0';

    if (::connect(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == -1)
    {
        const int captured = errno;
        ::close(sock);
        throw make_errno_error("Consumer",
                               ("connect to '" + endpoint + "' failed").c_str(),
                               captured);
    }

    pollfd pfd{sock, POLLIN, 0};
    const int n = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (n == 0)
    {
        ::close(sock);
        throw std::runtime_error(
            "ShmCapabilityConsumer: timeout waiting for capability fd from '" +
            endpoint + "'.");
    }
    if (n == -1)
    {
        const int captured = errno;
        ::close(sock);
        throw make_errno_error("Consumer", "poll failed", captured);
    }

    char   iov_byte = 0;
    iovec  iov{&iov_byte, 1};
    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;
    union
    {
        char     buf[CMSG_SPACE(sizeof(int))];
        cmsghdr  align;
    } u{};
    std::memset(u.buf, 0, sizeof(u.buf));
    msg.msg_control    = u.buf;
    msg.msg_controllen = sizeof(u.buf);

    const ssize_t got           = ::recvmsg(sock, &msg, 0);
    const int     recv_errno    = errno;
    // Close the socket unconditionally BEFORE any of the throw paths
    // below so a malformed recvmsg cannot leak the AF_UNIX fd.  The
    // capability fd we care about (if any) was already installed into
    // our process's fd table by the kernel as part of the SCM_RIGHTS
    // cmsg — we no longer need the socket for anything else.
    ::close(sock);

    if (got != 1)
    {
        throw std::runtime_error(
            "ShmCapabilityConsumer: recvmsg returned " + std::to_string(got) +
            " (expected 1 byte of regular payload + cmsg): " +
            std::strerror(recv_errno));
    }

    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
    {
        throw std::runtime_error(
            "ShmCapabilityConsumer: producer did not send an SCM_RIGHTS fd cmsg.");
    }

    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    if (fd < 0)
    {
        throw std::runtime_error(
            "ShmCapabilityConsumer: received invalid fd from producer.");
    }

    struct stat st{};
    if (::fstat(fd, &st) == -1)
    {
        const int captured = errno;
        ::close(fd);
        throw make_errno_error("Consumer", "fstat on received fd failed", captured);
    }

    const size_t segment_size = static_cast<size_t>(st.st_size);
    if (segment_size == 0)
    {
        ::close(fd);
        throw std::runtime_error(
            "ShmCapabilityConsumer: received fd has zero size — producer ftruncate skipped?");
    }

    void *base = ::mmap(nullptr, segment_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, /*offset=*/0);
    if (base == MAP_FAILED)
    {
        const int captured = errno;
        ::close(fd);
        throw make_errno_error("Consumer", "mmap of received fd failed", captured);
    }

    received_fd_ = fd;
    mmap_base_   = base;
    mmap_size_   = segment_size;
}

MemfdConsumer::~MemfdConsumer()
{
    if (mmap_base_ != nullptr)
    {
        ::munmap(mmap_base_, mmap_size_);
    }
    if (received_fd_ != -1)
    {
        ::close(received_fd_);
    }
}

std::span<std::byte>
MemfdConsumer::data()
{
    return {static_cast<std::byte *>(mmap_base_), mmap_size_};
}

size_t
MemfdConsumer::size() const noexcept
{
    return mmap_size_;
}

} // anonymous namespace

std::unique_ptr<IShmCapabilityProducer>
create_shm_capability_producer(size_t bytes)
{
    return std::make_unique<MemfdProducer>(bytes);
}

std::unique_ptr<IShmCapabilityConsumer>
attach_shm_capability_consumer(const std::string       &endpoint,
                               std::chrono::milliseconds timeout)
{
    return std::make_unique<MemfdConsumer>(endpoint, timeout);
}

#else  // FreeBSD / macOS / Windows / other — backend lands under later phases.

std::unique_ptr<IShmCapabilityProducer>
create_shm_capability_producer(size_t /*bytes*/)
{
    throw std::runtime_error(
        "HEP-CORE-0041 capability transport: no backend for this platform "
        "(Linux landed in task #249; FreeBSD + macOS + Windows pending).");
}

std::unique_ptr<IShmCapabilityConsumer>
attach_shm_capability_consumer(const std::string & /*endpoint*/,
                               std::chrono::milliseconds /*timeout*/)
{
    throw std::runtime_error(
        "HEP-CORE-0041 capability transport: no backend for this platform "
        "(Linux landed in task #249; FreeBSD + macOS + Windows pending).");
}

#endif

} // namespace pylabhub::utils::security
