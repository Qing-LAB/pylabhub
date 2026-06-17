#pragma once
/**
 * @file shm_capability_channel.hpp
 * @brief Cross-platform SHM capability transport — Layer 1.
 *
 * HEP-CORE-0041 §5-§6 + §9 D1 (Option A) + §10 Phase 1.
 *
 * Two concerns layered, deliberately separated:
 *
 *   - **L1 (this header) — transport mechanics.**  Create an anonymous
 *     shared-memory region; bind a Unix socket / named pipe; accept one
 *     peer; hand the kernel-issued fd / HANDLE to that peer.  Knows
 *     about OS primitives (memfd_create, SCM_RIGHTS, SHM_ANON,
 *     CreateFileMapping, DuplicateHandle).  Knows nothing about CURVE
 *     pubkeys, brokers, or allowlists.
 *
 *   - **L2 (lives elsewhere — see HEP-0041 §9 D4 attach sequence) —
 *     auth orchestration.**  Signed-nonce verification, SO_PEERCRED
 *     sanity check, broker CONSUMER_ATTACH_REQ pre-confirm,
 *     cache-vs-broker divergence WARN.  Uses L1 for the actual fd
 *     handoff but knows nothing about which OS primitive backs the
 *     transport.
 *
 * The split prevents the per-platform backend code (memfd / SHM_ANON /
 * CreateFileMapping) from each having to duplicate the auth flow, and
 * lets the auth orchestration be unit-tested without spinning up real
 * SHM regions.
 *
 * **Platform support in this header today.**  POSIX (Linux + FreeBSD +
 * macOS) — the AcceptedPeer struct uses `pid_t` / `uid_t` / `gid_t`
 * from `<sys/types.h>`.  Windows port lands with task #244's Phase 3;
 * at that point the AcceptedPeer shape will need a per-platform
 * variant (PID + token handle instead of POSIX creds).
 *
 * @see docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md §5-§6, §9 D1+D4, §10.
 * @see docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md §1
 *      Amendment 2026-06-16 (the supersession that motivates this module).
 */

#include "pylabhub_utils_export.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <sys/types.h>  // pid_t, uid_t, gid_t — POSIX SO_PEERCRED triple

namespace pylabhub::utils::security
{

/// Producer side of a capability channel.
///
/// Pure abstract interface.  Concrete backends register through
/// `create_shm_capability_producer()`; today only the Linux/FreeBSD
/// `memfd_create` backend is planned (task #249).  macOS uses
/// `SHM_ANON` (deferred), Windows uses `CreateFileMapping(NULL) +
/// DuplicateHandle` (deferred); each is its own backend class behind
/// the same factory.
///
/// **Ownership semantics.**  An instance owns:
///   - The anonymous SHM region's fd / HANDLE.
///   - The bound endpoint (Unix socket / named pipe listener).
///   - The mmap covering `size()` bytes accessible via `data()`.
///
/// Destructor releases all of the above.  Non-copyable + non-movable
/// to keep the kernel-resource ownership unambiguous.
///
/// **Threading.**  The interface itself is single-threaded per
/// instance: `bind_endpoint` → `accept_one` → `send_capability` is a
/// serial flow owned by one thread.  `data()` returns a span that
/// remains valid until destruction; concurrent access to the bytes is
/// the caller's responsibility (the consumer thread sees the same
/// region after fd handoff, and the producer/consumer share a ring
/// buffer protocol on top of `data()` per HEP-CORE-0002).
class PYLABHUB_UTILS_EXPORT IShmCapabilityProducer
{
public:
    /// Per-peer credentials surfaced after `accept_one()`.
    ///
    /// `peer_socket_fd` is the fd of the accepted Unix-socket
    /// connection — used to send the capability and (optionally)
    /// to read further bytes from the peer.  Lifetime is bounded by
    /// the producer instance; closing the producer closes accepted
    /// peer sockets.
    ///
    /// `pid` / `uid` / `gid` come from `SO_PEERCRED` (Linux) or its
    /// per-platform equivalent.  Used by L2's defence-in-depth sanity
    /// check (HEP-0041 §9 D4 step 3): the peer must be in the
    /// expected trust domain.
    struct AcceptedPeer
    {
        int   peer_socket_fd{-1};
        pid_t pid{};
        uid_t uid{};
        gid_t gid{};
    };

    virtual ~IShmCapabilityProducer() = default;

    IShmCapabilityProducer(const IShmCapabilityProducer &)            = delete;
    IShmCapabilityProducer &operator=(const IShmCapabilityProducer &) = delete;
    IShmCapabilityProducer(IShmCapabilityProducer &&)                 = delete;
    IShmCapabilityProducer &operator=(IShmCapabilityProducer &&)      = delete;

    /// Bind a listener at the given filesystem path (POSIX Unix
    /// socket) / pipe name (Windows).  Idempotent — calling twice
    /// fails the second time.  Returns true on success.
    ///
    /// Path discipline is the caller's responsibility: the broker
    /// publishes the endpoint to authorized consumers via
    /// `CONSUMER_REG_ACK` (HEP-0041 §5.3, finalized in task #254).
    virtual bool bind_endpoint(const std::string &endpoint) = 0;

    /// Block up to `timeout` waiting for one consumer connection.
    /// Returns the accepted peer's fd + kernel-checked credentials
    /// on success; `std::nullopt` on timeout.
    ///
    /// Returning `nullopt` is NOT an error: the caller polls back.
    /// Errors (socket closed, peer dropped, EAGAIN exhaustion) throw.
    [[nodiscard]] virtual std::optional<AcceptedPeer>
    accept_one(std::chrono::milliseconds timeout) = 0;

    /// Send the anonymous-SHM fd / HANDLE to the previously-accepted
    /// peer via `SCM_RIGHTS` (POSIX) or `DuplicateHandle` (Windows).
    ///
    /// Producer retains its own dup — the kernel duplicates the
    /// descriptor into the peer's process during sendmsg.  Returns
    /// true on successful send.  Caller is responsible for any
    /// surrounding auth flow (signed-nonce verify, broker
    /// pre-confirm) — this method is the unconditional handoff once
    /// the auth layer says "send it."
    virtual bool send_capability(int peer_socket_fd) = 0;

    /// Producer-side RW view of the anonymous SHM segment.  Span
    /// is stable across the producer's lifetime; bytes are visible
    /// to the consumer after `send_capability()` succeeds.
    [[nodiscard]] virtual std::span<std::byte> data() = 0;

    /// Segment size in bytes, fixed at construction.
    [[nodiscard]] virtual size_t size() const noexcept = 0;

protected:
    IShmCapabilityProducer() = default;
};

/// Consumer side of a capability channel.
///
/// Constructed by `attach_shm_capability_consumer()`: connects to the
/// producer's endpoint, receives the anonymous-SHM fd via
/// `SCM_RIGHTS`, and mmaps it.  Pure abstract for the same backend-
/// dispatch reasons as the producer.
class PYLABHUB_UTILS_EXPORT IShmCapabilityConsumer
{
public:
    virtual ~IShmCapabilityConsumer() = default;

    IShmCapabilityConsumer(const IShmCapabilityConsumer &)            = delete;
    IShmCapabilityConsumer &operator=(const IShmCapabilityConsumer &) = delete;
    IShmCapabilityConsumer(IShmCapabilityConsumer &&)                 = delete;
    IShmCapabilityConsumer &operator=(IShmCapabilityConsumer &&)      = delete;

    /// Consumer-side RW view of the segment received from the
    /// producer.  Span is stable until destruction.
    [[nodiscard]] virtual std::span<std::byte> data() = 0;

    /// Segment size in bytes — obtained from `fstat()` on the
    /// received fd (the producer's `ftruncate()` size).
    [[nodiscard]] virtual size_t size() const noexcept = 0;

protected:
    IShmCapabilityConsumer() = default;
};

/// Construct a producer-side capability channel backed by an
/// anonymous SHM segment of `bytes` bytes.
///
/// Throws `std::runtime_error` until task #249 lands the
/// Linux/FreeBSD `memfd_create` backend.  The throw is the contract
/// — see the L2 skeleton test for the test pin.
PYLABHUB_UTILS_EXPORT std::unique_ptr<IShmCapabilityProducer>
create_shm_capability_producer(size_t bytes);

/// Connect to a producer endpoint, receive its SHM fd via
/// `SCM_RIGHTS`, and map it RW.  Blocks up to `timeout` for the
/// receive.
///
/// Throws `std::runtime_error` until task #249 lands the backend.
PYLABHUB_UTILS_EXPORT std::unique_ptr<IShmCapabilityConsumer>
attach_shm_capability_consumer(const std::string       &endpoint,
                               std::chrono::milliseconds timeout);

} // namespace pylabhub::utils::security
