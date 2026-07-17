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
#include "utils/logger.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(PYLABHUB_PLATFORM_LINUX) || defined(PYLABHUB_PLATFORM_FREEBSD) || defined(PYLABHUB_PLATFORM_APPLE)
#    include <fcntl.h>
#    include <poll.h>
#    include <sys/mman.h>
#    include <sys/socket.h>
#    include <time.h>       // clock_gettime — #321 atomic-rename bind (2026-07-03)
#    include <fcntl.h>      // AT_FDCWD — #321 fix (renameat2 RENAME_NOREPLACE)
#    include <sys/syscall.h> // SYS_renameat2 — Linux 3.15+ (2014)
#    include <linux/fs.h>   // RENAME_NOREPLACE flag
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <sys/un.h>
#    include <unistd.h>
#endif

#if defined(PYLABHUB_PLATFORM_FREEBSD)
#    include <sys/ucred.h>     // xucred (FreeBSD-specific)
#endif

#if defined(PYLABHUB_PLATFORM_APPLE)
#    include <sys/random.h>    // getentropy for the anonymous-shm name trick
#endif

// <windows.h> already pulled in by plh_platform.hpp on Windows builds.

namespace pylabhub::utils::security
{

// HEP-CORE-0041 §5.1 — see header docstring.  Cross-platform: every
// backend that uses an AF_UNIX-style filesystem path needs the bare
// path, not the canonical URI form.
std::string
strip_unix_scheme(std::string_view endpoint)
{
    constexpr std::string_view kScheme = "unix://";
    if (endpoint.size() >= kScheme.size() &&
        endpoint.substr(0, kScheme.size()) == kScheme)
    {
        return std::string{endpoint.substr(kScheme.size())};
    }
    return std::string{endpoint};
}

#if defined(PYLABHUB_PLATFORM_LINUX)

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
    [[nodiscard]] int           borrow_fd() const noexcept override { return anon_fd_; }

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

    // HEP-CORE-0041 §5.1: the canonical endpoint shape carries a
    // `unix://` URI scheme (as emitted by
    // `default_shm_capability_endpoint`).  `sockaddr_un::sun_path`
    // needs the bare filesystem path — strip the scheme here so the
    // backend accepts both URI form (production) and bare paths
    // (existing L2 tests).
    const std::string sock_path = strip_unix_scheme(endpoint);
    if (sock_path.empty())
    {
        return false;
    }

    // sun_path is a fixed-size array; reject too-long paths at the boundary.
    sockaddr_un addr{};
    if (sock_path.size() >= sizeof(addr.sun_path))
    {
        return false;
    }

    // HEP-CORE-0041 §5.1 hardening contract (header line 310):
    // ensure the parent dir exists at mode 0700.  On systemd-managed
    // hosts `${XDG_RUNTIME_DIR}` is itself mode 0700 (via
    // `pam_systemd.so`); the `pylabhub/` subdir layered under it
    // must be created by us.  Idempotent — `create_directories` is
    // a no-op if the path already exists.  Errors are swallowed
    // here; `bind(2)` below will surface the resulting
    // `ENOENT`/`EACCES` if the parent setup did not take.
    try
    {
        const std::filesystem::path parent =
            std::filesystem::path{sock_path}.parent_path();
        if (!parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (!ec)
            {
                std::filesystem::permissions(
                    parent,
                    std::filesystem::perms::owner_all,
                    std::filesystem::perm_options::replace,
                    ec);
            }
        }
    }
    catch (...)
    {
        // Best-effort — bind(2) below surfaces the real failure.
    }

    // Populate sockaddr BEFORE the live-peer probe so the probe and
    // the bind both target the same path.
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size());
    addr.sun_path[sock_path.size()] = '\0';

    const int sock = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock == -1)
    {
        return false;
    }

    // HEP-CORE-0041 1i-prod-hardening H3d — probe-then-refuse-if-live.
    // The probe is a safety gate to detect a working producer at the
    // target path and refuse to clobber it (two role hosts picking
    // the same channel name; same-uid race that SO_PEERCRED doesn't
    // catch).  Behaviour:
    //   - connect() succeeds → live peer is already bound.  Refuse.
    //   - connect() fails with ECONNREFUSED → stale socket file (path
    //     exists, listener gone).  Unlink it; the RENAME_NOREPLACE
    //     rename below then succeeds on the empty target.
    //   - connect() fails with ENOENT → no path; nothing to unlink.
    //   - any other errno → can't determine state; refuse conservatively.
    bool target_needs_unlink = false;
    {
        const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (probe == -1)
        {
            ::close(sock);
            return false;
        }
        const int rc = ::connect(probe,
                                  reinterpret_cast<const sockaddr *>(&addr),
                                  sizeof(addr));
        const int captured_errno = errno;
        ::close(probe);
        if (rc == 0)
        {
            // Live peer responded.  Refuse to clobber.
            ::close(sock);
            return false;
        }
        if (captured_errno == ECONNREFUSED)
        {
            // Stale socket file at target (prior producer crashed
            // without cleanup, or systemd killed it).  Unlink it
            // before rename(NOREPLACE) so the target is empty.
            target_needs_unlink = true;
        }
        else if (captured_errno != ENOENT)
        {
            // Unknown error (EACCES, EINVAL, …) — can't safely proceed.
            ::close(sock);
            return false;
        }
    }

    // #321 (2026-07-03) — atomic-rename bind.  Replaces the earlier
    // unlink → bind sequence which had a TOCTOU: between our unlink
    // and our bind, a hostile local process (or a racing legitimate
    // process on the same uid) could bind at the target path,
    // causing our bind to fail with EADDRINUSE.  DoS on producer
    // startup.
    //
    // Fix: bind to a unique temp path in the same directory, then
    // rename(2) atomically into place.  rename(2) on POSIX overwrites
    // the destination unconditionally in a single syscall — no race
    // window between "verify path is safe" and "occupy path."  Nobody
    // can slip in.
    //
    // Temp-name uniqueness: PID + monotonic-clock nanoseconds.  On
    // POSIX, two threads in the same PID getting the same nsec is
    // astronomically unlikely; a same-machine attacker predicting
    // this exact pair to plant a symlink is infeasible.
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const std::string temp_path =
        sock_path + ".tmp-" + std::to_string(::getpid()) + "-" +
        std::to_string(static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                        static_cast<uint64_t>(ts.tv_nsec));
    if (temp_path.size() >= sizeof(addr.sun_path))
    {
        // Temp name overflows sun_path.  Extremely unlikely given
        // realistic channel names, but bail cleanly.
        ::close(sock);
        return false;
    }
    sockaddr_un temp_addr{};
    temp_addr.sun_family = AF_UNIX;
    std::memcpy(temp_addr.sun_path, temp_path.c_str(), temp_path.size());
    temp_addr.sun_path[temp_path.size()] = '\0';

    if (::bind(sock, reinterpret_cast<const sockaddr *>(&temp_addr),
                sizeof(temp_addr)) == -1)
    {
        ::close(sock);
        return false;
    }

    // chmod before rename so target inherits the tight mode from the
    // moment it appears at the final path.  This is NOT the security
    // gate — the cryptographic handshake (HEP-CORE-0041 §5.3) is —
    // but defence in depth against another local UID trying to
    // connect.
    ::chmod(temp_path.c_str(), 0700);

    // If the probe reported a stale socket file at the target
    // (ECONNREFUSED path above set target_needs_unlink=true), unlink
    // it now so the RENAME_NOREPLACE rename below sees an empty
    // target and succeeds.  Any concurrent racer who binds between
    // this unlink and our rename will be caught by RENAME_NOREPLACE
    // failing with EEXIST (the correct outcome — refuse to clobber
    // a live listener even in the same-channel-name misconfig case).
    if (target_needs_unlink)
    {
        ::unlink(sock_path.c_str());  // best-effort; racer may have won
    }

    // Atomic move into the target path.  Uses renameat2(2) with
    // RENAME_NOREPLACE so the rename FAILS with EEXIST if another
    // process bound at sock_path between our probe (line ~254) and
    // here.  Preserves the pre-#321 loud-fail semantics on
    // same-channel-name collisions — two producers both starting up
    // with the same channel name now: the second one's rename returns
    // EEXIST, its bind_endpoint() returns false, its caller sees the
    // error, instead of silently clobbering the first producer's
    // socket and leaving it bound to an orphaned inode (2026-07-03
    // code review Finding #2).
    //
    // Linux 3.15+ (2014) is required for renameat2; the syscall
    // interface is used directly rather than the glibc wrapper so we
    // don't depend on glibc >= 2.28.
    if (::syscall(SYS_renameat2, AT_FDCWD, temp_path.c_str(), AT_FDCWD,
                   sock_path.c_str(), RENAME_NOREPLACE) == -1)
    {
        // EEXIST → another process won the bind race; refuse rather
        // than clobber.  Any other errno → unexpected rename failure.
        // Both cases surface as bind_endpoint() returning false; the
        // caller's diagnostic points at the endpoint contention.
        ::unlink(temp_path.c_str());  // best-effort cleanup
        ::close(sock);
        return false;
    }

    if (::listen(sock, /*backlog=*/8) == -1)
    {
        ::close(sock);
        ::unlink(sock_path.c_str());
        return false;
    }

    listen_fd_  = sock;
    // Store the stripped path: the dtor unlinks `bound_path_`, and the
    // kernel-side cleanup needs the bare filesystem path.
    bound_path_ = sock_path;
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

    // #322 (2026-07-02) — retry accept4 on EINTR.  The sister ::poll
    // above already returns nullopt on EINTR so the caller's outer
    // loop retries; without an EINTR retry HERE, a signal delivered
    // between poll returning ready and accept4 completing throws and
    // drops the consumer's connection attempt entirely.  Sibling fix
    // of task #304 (recvmsg/sendmsg EINTR) — same syscall family,
    // missed in #304's scope.  Retry loop is bounded implicitly by
    // the caller's outer timeout — a persistent EINTR storm would
    // eventually surface via that outer bound.
    int peer;
    for (;;)
    {
        peer = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (peer >= 0) break;
        const int captured = errno;
        if (captured == EINTR) continue;
        throw make_errno_error("Producer::accept_one", "accept4 failed",
                                captured);
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

    // EINTR retry: a signal delivered while sendmsg is in flight
    // (SIGCHLD from a spawned helper, SIGPROF from a profiler timer,
    // debug signals, SIGUSR1 from ThreadManager) returns -1 with
    // errno=EINTR.  Without retry the producer-side fd handover
    // spuriously fails despite both peers being healthy.  The
    // SCM_RIGHTS payload is a single byte + one cmsg — small enough
    // that a partial send returning sent != 1 != -1 is impossible
    // on a SOCK_DGRAM-like AF_UNIX socket; the only retry-worthy
    // case is sent == -1 with errno == EINTR.  Origin: 2026-06-30
    // workflow code-review finding [6] PLAUSIBLE, task #304.
    ssize_t sent;
    do
    {
        sent = ::sendmsg(peer_socket_fd, &msg, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
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

/// Consumer-side: receive SHM fd via SCM_RIGHTS, fstat the size, mmap.
/// All set-up happens in the ctor; the AF_UNIX socket is closed before
/// the ctor returns because we no longer need it (the kernel duped the
/// capability fd into our process during recvmsg).
///
/// Two construction modes:
///   1. (endpoint, timeout): connect + recv.  Used by the legacy
///      L1-direct factory `attach_shm_capability_consumer` — useful in
///      L2 tests where the §5.5 ZAP-CURVE handshake is bypassed.
///   2. (socket_fd, timeout): recv on an ALREADY-CONNECTED + POST-§5.5-
///      HANDSHAKE socket.  Used by the production-path factory
///      `attach_shm_capability_consumer_from_socket` (HEP-0041 1i-mig-4
///      #272 consumer dial).  Takes ownership of `socket_fd`: closes it
///      before return on every path.
class MemfdConsumer final : public IShmCapabilityConsumer
{
public:
    MemfdConsumer(const std::string &endpoint, std::chrono::milliseconds timeout);
    MemfdConsumer(int socket_fd, std::chrono::milliseconds timeout);
    ~MemfdConsumer() override;

    std::span<std::byte> data() override;
    [[nodiscard]] size_t size() const noexcept override;
    [[nodiscard]] int    borrow_fd() const noexcept override { return received_fd_; }

private:
    /// Poll + recvmsg + fstat + mmap on an already-connected socket.
    /// Takes ownership of `sock_fd` (closes it before return on every
    /// path: success, timeout, recv failure, exception).  Stores the
    /// received fd + mmap into the member fields on success.  Throws
    /// std::runtime_error on any failure.  `diag_tag` is appended to
    /// the timeout error message for caller-side observability (e.g.
    /// "from 'unix:///run/.../shmcap-foo.sock'" or "from socket").
    void recv_and_mmap_owning_socket_(int                       sock_fd,
                                      std::chrono::milliseconds timeout,
                                      const std::string        &diag_tag);

    int    received_fd_{-1};
    void  *mmap_base_{nullptr};
    size_t mmap_size_{0};
};

void
MemfdConsumer::recv_and_mmap_owning_socket_(int                       sock_fd,
                                            std::chrono::milliseconds timeout,
                                            const std::string        &diag_tag)
{
    // RAII close for the AF_UNIX socket: the kernel duplicates the
    // capability fd into our process table during recvmsg, so after
    // we have it we no longer need our local end of the AF_UNIX
    // connection.  This guard ALSO covers all throw paths below, so a
    // malformed recvmsg cannot leak the socket fd.
    struct SockCloser
    {
        int fd;
        ~SockCloser() { if (fd >= 0) ::close(fd); }
    } closer{sock_fd};

    pollfd pfd{sock_fd, POLLIN, 0};
    const int n = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (n == 0)
    {
        throw std::runtime_error(
            "ShmCapabilityConsumer: timeout waiting for capability fd " +
            diag_tag);
    }
    if (n == -1)
    {
        throw make_errno_error("Consumer", "poll failed", errno);
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

    // EINTR retry — symmetric with the producer-side sendmsg loop
    // above.  Without retry, any signal delivered to the consumer's
    // role process while it's parked in recvmsg (waiting up to
    // 2000 ms for the producer's SCM_RIGHTS message) turns a
    // healthy in-flight fd handover into a hard handshake failure,
    // and the role propagates that failure up through
    // `apply_consumer_reg_ack_shm_` → consumer registration fails.
    // Same single-byte payload rationale as send side: partial recv
    // returning got != 1 != -1 is not a real possibility.  Origin:
    // 2026-06-30 workflow code-review finding [5] PLAUSIBLE, task #304.
    ssize_t got;
    do
    {
        got = ::recvmsg(sock_fd, &msg, 0);
    } while (got < 0 && errno == EINTR);
    const int recv_errno = errno;

    if (got != 1)
    {
        throw std::runtime_error(
            "ShmCapabilityConsumer: recvmsg returned " + std::to_string(got) +
            " (expected 1 byte of regular payload + cmsg): " +
            std::strerror(recv_errno));
    }

    // Fail closed on a truncated control message.  MSG_CTRUNC means the
    // kernel dropped part of the ancillary data — e.g. a sender that
    // attached more than one SCM_RIGHTS fd (the kernel installs only the
    // first and discards the rest, so no fd leaks, but an honest producer
    // sends exactly one).  Reject the anomalous message rather than
    // silently accept the first fd of it.  REVIEW-C (#276), 2026-07-16.
    if (msg.msg_flags & MSG_CTRUNC)
    {
        throw std::runtime_error(
            "ShmCapabilityConsumer: truncated ancillary data (MSG_CTRUNC) — "
            "malformed/multi-fd SCM_RIGHTS message from producer.");
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

MemfdConsumer::MemfdConsumer(const std::string       &endpoint,
                             std::chrono::milliseconds timeout)
{
    // HEP-CORE-0041 §5.1: the producer publishes the canonical
    // `unix://` URI in CONSUMER_REG_ACK; strip the scheme before
    // `connect(2)` (sockaddr_un::sun_path needs the bare filesystem
    // path).  Symmetric with MemfdProducer::bind_endpoint.
    const std::string sock_path = strip_unix_scheme(endpoint);

    sockaddr_un addr{};
    if (sock_path.empty() || sock_path.size() >= sizeof(addr.sun_path))
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
    std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size());
    addr.sun_path[sock_path.size()] = '\0';

    if (::connect(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == -1)
    {
        const int captured = errno;
        ::close(sock);
        throw make_errno_error("Consumer",
                               ("connect to '" + endpoint + "' failed").c_str(),
                               captured);
    }

    recv_and_mmap_owning_socket_(sock, timeout, "from '" + endpoint + "'.");
}

MemfdConsumer::MemfdConsumer(int                       socket_fd,
                             std::chrono::milliseconds timeout)
{
    // Production path: the §5.5 ZAP-CURVE handshake has already
    // completed on `socket_fd` (see attach_protocol.cpp
    // initiate_consumer_handshake).  We just poll + recv the
    // SCM_RIGHTS message + mmap.  Takes ownership of socket_fd
    // unconditionally via recv_and_mmap_owning_socket_'s RAII closer.
    if (socket_fd < 0)
    {
        throw std::invalid_argument(
            "ShmCapabilityConsumer: socket_fd must be >= 0 "
            "(callers must pass an already-connected + post-§5.5-handshake socket).");
    }
    recv_and_mmap_owning_socket_(socket_fd, timeout, "from socket.");
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

std::unique_ptr<IShmCapabilityConsumer>
attach_shm_capability_consumer_from_socket(int                       socket_fd,
                                           std::chrono::milliseconds timeout)
{
    return std::make_unique<MemfdConsumer>(socket_fd, timeout);
}

// HEP-CORE-0041 substep 1g (#254) — see header for the contract.
//
// Linux: prefer ${XDG_RUNTIME_DIR}/pylabhub/shmcap-<channel>.sock
// (kernel-enforced mode-0700 per-user isolation via systemd
// pam_systemd.so).  Fall back to /tmp/pylabhub-shmcap-<uid>-<channel>.sock
// on non-systemd hosts where the env var is unset — `/tmp` is mode 1777
// world-readable so we embed the uid in the filename to avoid
// cross-user collisions, and we log a startup-time WARN so the operator
// notices the weaker isolation.
//
// Caller responsibilities (deferred to the substep that wires
// IShmCapabilityProducer::bind_endpoint):
//   - mkdir -p the `pylabhub/` subdir at mode 0700 under XDG_RUNTIME_DIR
//     before bind (parent dir already mode 0700 from systemd);
//   - on /tmp fallback, no mkdir needed — bind writes the socket file
//     directly with the producer's umask applied.
std::string default_shm_capability_endpoint(std::string_view channel_name)
{
    if (channel_name.empty())
    {
        throw std::invalid_argument(
            "default_shm_capability_endpoint: channel_name must be non-empty");
    }
    // Defence in depth — reject path separators and the `unix://` scheme
    // separator so the returned path can't be hijacked by a misconfigured
    // channel name.  Broker-side channel-name validation should catch
    // these earlier, but the helper enforces its own input contract.
    if (channel_name.find('/') != std::string_view::npos ||
        channel_name.find(':') != std::string_view::npos)
    {
        throw std::invalid_argument(
            "default_shm_capability_endpoint: channel_name must not contain "
            "'/' or ':' (HEP-CORE-0021 channel-name grammar)");
    }

    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg != nullptr && xdg[0] != '\0')
    {
        std::string path = "unix://";
        path += xdg;
        path += "/pylabhub/shmcap-";
        path += channel_name;
        path += ".sock";
        return path;
    }

    // Fallback — log once per process so operators on non-systemd hosts
    // know the per-user isolation is weaker.  Note: LOGGER may not be
    // initialised at the very first call (early process startup), so a
    // one-shot atomic flag guards it.
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_acq_rel))
    {
        LOGGER_WARN(
            "[ShmCapability] XDG_RUNTIME_DIR unset; falling back to "
            "/tmp/pylabhub-shmcap-<uid>-<channel>.sock for SHM-capability "
            "endpoints.  This deployment has weaker per-user isolation than "
            "a systemd-managed host (HEP-CORE-0041 §5.1 hardening note).");
    }

    std::string path = "unix:///tmp/pylabhub-shmcap-";
    path += std::to_string(::getuid());
    path += "-";
    path += channel_name;
    path += ".sock";
    return path;
}

#endif // PYLABHUB_PLATFORM_LINUX


// ============================================================================
//  FreeBSD backend — PLAN (not implemented; #error fires below if you
//  try to build this lib on FreeBSD).  Task #259 tracks the work.
//
//  ARCHITECTURE.  FreeBSD ≥13.0 has nearly the same primitives as
//  Linux for this purpose.  The producer creates an anonymous SHM
//  region via `memfd_create`, binds an AF_UNIX/SOCK_STREAM listener,
//  accepts a consumer connection, reads kernel-attested credentials
//  via `getpeereid`, and hands the fd to the consumer via `sendmsg`
//  with an `SCM_RIGHTS` cmsg.  Consumer's flow mirrors the Linux
//  version: connect, recvmsg for the fd, fstat, mmap.
//
//  SUBSTITUTIONS FROM LINUX:
//    - `getpeereid(fd, *uid, *gid)` replaces
//      `getsockopt(SO_PEERCRED, struct ucred)`.  FreeBSD's `xucred`
//      does NOT carry the peer PID, so `AcceptedPeer.pid` stays at
//      its default 0 — document in any L2 caller that consumes pid
//      on this platform.
//    - `<sys/ucred.h>` is the home of `xucred` (already included
//      conditionally above; defensive).  `getpeereid` itself lives
//      in `<unistd.h>`.
//    - `memfd_create(name, MFD_CLOEXEC)` — same signature as Linux,
//      same `<sys/mman.h>` header.  Present since FreeBSD 13.0;
//      pre-13.0 platforms must error out at runtime in the ctor
//      (or this #error should narrow the platform predicate to
//      `defined(PYLABHUB_PLATFORM_FREEBSD) && __FreeBSD_version >= 1300000`).
//    - `accept4(SOCK_CLOEXEC)` — available since FreeBSD 10.0.
//    - `sendmsg` with `MSG_NOSIGNAL` — supported.
//
//  EXPECTED PITFALLS (validate at task #259):
//    - errno values from these syscalls may differ from Linux.  The
//      `make_errno_error` helper above uses `strerror` so the message
//      is portable, but any caller that branches on numeric `errno`
//      values needs platform-aware handling.
//    - `EINTR` semantics on `poll(2)`: same return convention as Linux.
//    - `MFD_ALLOW_SEALING` flag: present on FreeBSD too; not used here
//      but worth knowing if a future hardening pass wants to seal
//      writes.
//
//  L2 INTEGRATION.  Substep 1c's AttachProtocol will work without
//  modification on FreeBSD once this backend ships — it consumes
//  `AcceptedPeer.uid` (populated by `getpeereid`) and ignores `pid`
//  when zero.  The L2 `peer_socket_fd` ownership semantic from the
//  Linux backend carries over: caller owns the fd, producer does
//  not track or close.
//
//  HOW TO IMPLEMENT.  Copy the Linux block above, change the
//  `getsockopt(SO_PEERCRED, ...)` call inside `accept_one` to
//  `getpeereid`, leave the other code paths intact, run the L2
//  `RoundTrip_ConsumerSeesProducerWrites` test on a real FreeBSD
//  host, and only then remove this `#error`.  Update task #259 with
//  validation evidence.
// ============================================================================

#if defined(PYLABHUB_PLATFORM_FREEBSD)
#    error "HEP-CORE-0041 FreeBSD backend not implemented (task #259). See plan in shm_capability_channel.cpp above this line. Refusing to compile until implemented."
#endif // PYLABHUB_PLATFORM_FREEBSD


// ============================================================================
//  macOS backend — PLAN (not implemented; #error fires below if you
//  try to build this lib on macOS).  Task #260 tracks the work.
//
//  ARCHITECTURE.  macOS / Darwin is BSD-derived but lacks
//  `memfd_create` and the FreeBSD-only `SHM_ANON` extension that
//  HEP-CORE-0041 §13 incorrectly attributes to macOS.  The producer
//  emulates anonymous SHM by creating a named SHM segment with a
//  random name and immediately unlinking it — the fd remains valid
//  and `mmap`-able, the name disappears from the kernel namespace,
//  and no other process can `shm_open` it.  AF_UNIX/SOCK_STREAM
//  listener + `SCM_RIGHTS` fd handoff work identically to Linux at
//  the calling-convention level.
//
//  ANON-SHM EMULATION (the only non-trivial substitution):
//    1. Generate a 24-char random name (`/plh_<22 hex>`) — macOS
//       `PSHMNAMLEN` typically caps names at 31, leaving headroom.
//       Use `getentropy` (BSD) or `arc4random_buf` for the random
//       bytes.
//    2. `shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600)` — fails
//       cleanly on collision so retry with a fresh name.
//    3. `shm_unlink(name)` IMMEDIATELY.  The fd survives the unlink;
//       the name is gone.  This is the standard portable
//       "anonymous SHM" trick.
//    4. `ftruncate` + `mmap` proceed exactly as on Linux.
//
//  OTHER SUBSTITUTIONS FROM LINUX:
//    - No `accept4`.  Use `accept()` and immediately follow with
//      `fcntl(F_SETFD, FD_CLOEXEC)` on the returned fd.  Acceptable
//      race window because the spawned fd inherits CLOEXEC from
//      its listening socket if `SOCK_CLOEXEC` was set at socket()
//      time (which it is — both `socket()` calls use `SOCK_STREAM |
//      SOCK_CLOEXEC` and macOS honors that flag).  The `fcntl` here
//      is belt-and-braces.
//    - `getpeereid(fd, *uid, *gid)` for cred (same as FreeBSD; no
//      PID surfaced).
//    - `SCM_RIGHTS` works as on Linux (BSD-derived).
//
//  EXPECTED PITFALLS (validate at task #260):
//    - macOS `shm_open` mode is honored, but the segment is created
//      under `/var/db/shm/` style storage; `umask` may further
//      restrict.  0600 should give owner-only access.
//    - `PSHMNAMLEN` is technically platform-defined; 31 is the
//      historical Darwin value.  Pin in the impl rather than relying
//      on the constant existing in <sys/posix_shm.h>.
//    - libc's `MAP_FAILED` is `((void*)-1)` everywhere POSIX, but
//      worth a sanity assert.
//
//  L2 INTEGRATION.  Same shape as the Linux/FreeBSD backends — the
//  L2 AttachProtocol from substep 1c sees an `AcceptedPeer` with
//  `peer_socket_fd` + `uid`/`gid` populated and `pid` = 0.
//
//  HOW TO IMPLEMENT.  Copy the Linux block above.  Replace the
//  `memfd_create` call in the producer ctor with the shm_open +
//  shm_unlink dance from steps 1-3.  Replace the
//  `getsockopt(SO_PEERCRED)` call with `getpeereid`.  Replace
//  `accept4(SOCK_CLOEXEC)` with `accept()` + `fcntl(F_SETFD,
//  FD_CLOEXEC)`.  Validate against the L2 round-trip test on a
//  real macOS host before removing this `#error`.
// ============================================================================

#if defined(PYLABHUB_PLATFORM_APPLE)
#    error "HEP-CORE-0041 macOS backend not implemented (task #260). See plan in shm_capability_channel.cpp above this line. Refusing to compile until implemented."
#endif // PYLABHUB_PLATFORM_APPLE


// ============================================================================
//  Windows backend — PLAN (not implemented; #error fires below if you
//  try to build this lib on Windows).  Task #261 tracks the work.
//
//  ARCHITECTURE.  Windows uses a different IPC model.  Unix sockets
//  don't exist; named pipes substitute.  `SCM_RIGHTS` fd-passing
//  doesn't exist; `DuplicateHandle` between processes is the
//  equivalent kernel-mediated capability transfer.  The conceptual
//  shape is the same — producer publishes an endpoint, consumer
//  connects, producer hands a kernel-attested reference to its
//  anonymous SHM region — but every primitive is different.
//
//  PRIMITIVE MAPPING (Linux POSIX → Win64 equivalent):
//    - `bind` + `listen`(AF_UNIX) → `CreateNamedPipeA(L"\\\\.\\pipe\\
//      plh_shm_<random>", PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE
//      | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
//      PIPE_UNLIMITED_INSTANCES, in_buf, out_buf, default_timeout,
//      NULL_SECURITY_ATTRIBUTES)`.
//    - `accept`+timeout → `ConnectNamedPipe(handle, &overlapped)` +
//      `WaitForSingleObject(overlapped.hEvent, timeout_ms)`.  If
//      WAIT_TIMEOUT, cancel the I/O and return nullopt.
//    - `SO_PEERCRED` → `GetNamedPipeClientProcessId(pipe, &peer_pid)`.
//      No uid/gid concept on Windows; L2 attestation will use
//      process-token introspection in a separate substep.
//    - `memfd_create` → `CreateFileMappingA(INVALID_HANDLE_VALUE,
//      NULL, PAGE_READWRITE, size_hi, size_lo, NULL)`.  Same trick
//      pylabhub::platform::shm_create uses (see platform.cpp:402);
//      INVALID_HANDLE_VALUE source means pagefile-backed (no on-disk
//      name).  Caller maps via `MapViewOfFile`.
//    - `mmap` → `MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0,
//      size)`.  Cleanup: `UnmapViewOfFile` + `CloseHandle`.
//    - `sendmsg`+`SCM_RIGHTS` → the multi-step DuplicateHandle dance:
//        1. `OpenProcess(PROCESS_DUP_HANDLE, FALSE, peer_pid)` to
//           get a handle to the peer's process.
//        2. `DuplicateHandle(GetCurrentProcess(), section_handle,
//                            peer_process_handle, &dup_in_peer,
//                            0, FALSE, DUPLICATE_SAME_ACCESS)`.
//           The output `dup_in_peer` is a HANDLE VALID IN THE PEER
//           PROCESS — its numeric value is meaningful only there.
//        3. `WriteFile(pipe, &dup_in_peer, sizeof(HANDLE), ...)`.
//           Consumer reads via `ReadFile` and uses the numeric value
//           directly as a HANDLE in its address space.
//        4. `CloseHandle(peer_process_handle)`.
//    - `poll(POLLIN, timeout)` → `WaitForSingleObject(event_handle,
//      timeout_ms)` on the overlapped I/O event.
//    - `errno` → `GetLastError()` with `FormatMessageA` for human-
//      readable strings.  Replace `make_errno_error` with
//      `make_last_error_error` that wraps both.
//
//  PUBLIC-HEADER CHANGES (already landed, see header diff above):
//    - `AcceptedPeer` Windows variant: `void *peer_pipe_handle` +
//      `unsigned long peer_pid` (no uid/gid/socket_fd fields).
//    - `send_capability(void*, unsigned long)` signature taking the
//      pipe HANDLE and peer PID (NOT a fd).
//
//  EXPECTED PITFALLS (validate at task #261):
//    - `DuplicateHandle` source vs target argument order is easy to
//      reverse; the impl must triple-check against MSDN.
//    - `WaitForSingleObject` on the overlapped event must be paired
//      with `CancelIoEx` if the wait times out, or the next
//      ConnectNamedPipe call panics.
//    - `PIPE_REJECT_REMOTE_CLIENTS` flag should be set to prevent
//      remote SMB clients from connecting (Windows allows this by
//      default; security-relevant for the L2 trust-domain check).
//    - SECURITY_ATTRIBUTES — passing NULL gives the default DACL
//      which on workstations is "owner + admin only" but on domain-
//      joined hosts may be looser.  Coordinate with task #120
//      (Windows pathway hardening) for a proper SA construction.
//    - HANDLE size is 8 bytes on x64; the project requires Win64 so
//      `WriteFile(&handle, sizeof(HANDLE), ...)` is portable across
//      the 32→64 boundary that doesn't apply here.
//    - The HANDLE passed via WriteFile is a 64-bit integer with no
//      endian gotchas within a single machine.
//
//  L2 INTEGRATION.  Substep 1c's AttachProtocol will need a
//  per-platform branch:
//    - POSIX: read hello bytes from `AcceptedPeer.peer_socket_fd`.
//    - Win64: read hello bytes from `AcceptedPeer.peer_pipe_handle`
//      (cast to HANDLE; use ReadFile with overlapped+timeout).
//  This branch is already structurally present in the header (the
//  field-name differences force the caller to branch).  The L2 code
//  itself stays in the same file, behind a `#if defined(PYLABHUB_IS_
//  WINDOWS)` block.
//
//  HOW TO IMPLEMENT.  Greenfield — do NOT copy the Linux block;
//  the model is too different.  Write fresh, following the primitive
//  mapping table above.  Validate against the L2 round-trip test
//  on a real Windows host (and ensure the test itself adapts to the
//  HANDLE-based AcceptedPeer variant).  Coordinate with task #120
//  for SECURITY_ATTRIBUTES hardening before declaring complete.
// ============================================================================

#if defined(PYLABHUB_PLATFORM_WIN64)
#    error "HEP-CORE-0041 Windows backend not implemented (task #261). See plan in shm_capability_channel.cpp above this line. Refusing to compile until implemented."
#endif // PYLABHUB_PLATFORM_WIN64


// ============================================================================
//  Truly unsupported platform (e.g., PYLABHUB_PLATFORM_UNKNOWN).
//  Runtime throw — distinct from the #error blocks above which fire
//  at compile time for the target platforms we expect to support
//  eventually.  This block only matters if plh_platform.hpp tagged
//  the build host as UNKNOWN, in which case factory invocation
//  yields a clear runtime error rather than a link failure.
// ============================================================================

#if !defined(PYLABHUB_PLATFORM_LINUX)   && \
    !defined(PYLABHUB_PLATFORM_FREEBSD) && \
    !defined(PYLABHUB_PLATFORM_APPLE)   && \
    !defined(PYLABHUB_PLATFORM_WIN64)

std::unique_ptr<IShmCapabilityProducer>
create_shm_capability_producer(size_t /*bytes*/)
{
    throw std::runtime_error(
        "HEP-CORE-0041 capability transport: no backend for this platform "
        "(plh_platform.hpp did not detect Linux / FreeBSD / macOS / Win64).");
}

std::unique_ptr<IShmCapabilityConsumer>
attach_shm_capability_consumer(const std::string & /*endpoint*/,
                               std::chrono::milliseconds /*timeout*/)
{
    throw std::runtime_error(
        "HEP-CORE-0041 capability transport: no backend for this platform.");
}

std::unique_ptr<IShmCapabilityConsumer>
attach_shm_capability_consumer_from_socket(int /*socket_fd*/,
                                           std::chrono::milliseconds /*timeout*/)
{
    throw std::runtime_error(
        "HEP-CORE-0041 capability transport: no backend for this platform.");
}

#endif

} // namespace pylabhub::utils::security
