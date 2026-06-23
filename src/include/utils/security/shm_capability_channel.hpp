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
 *     auth orchestration.**  crypto_box challenge-response (HEP-CORE-0041 §5.5; the pre-1c signed-nonce model was cryptographically broken and was retired in the 2026-06-17 §9 D4 amendment), SO_PEERCRED
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
 * **Platform support today.**  The header surfaces the cross-platform
 * shape (per-platform `AcceptedPeer` variant; per-platform
 * `send_capability` signature).  The implementation file has the
 * full architectural plan for each backend as a detailed comment
 * block, with a `#error` directive that fires at compile time on any
 * target platform without a real implementation.  This prevents
 * shipping a binary that silently does nothing on platforms whose
 * code paths haven't been written yet.
 *
 *   - **Linux:** ✅ Working backend (task #249's `memfd_create` +
 *     `SO_PEERCRED` + `SCM_RIGHTS`).  Production-ready.
 *   - **FreeBSD ≥13.0:** ⛔ `#error` — design plan documented in the
 *     .cpp; implementation tracked under task **#259**.  Near-clone
 *     of the Linux backend with `getpeereid()` swapped in for
 *     `SO_PEERCRED` (FreeBSD's `xucred` carries no PID).
 *   - **macOS:** ⛔ `#error` — design plan documented in the .cpp;
 *     implementation tracked under task **#260**.  macOS lacks
 *     `memfd_create`; the design uses the `shm_open`+`shm_unlink`
 *     trick for anonymous SHM, no `accept4` (so `accept`+`fcntl`),
 *     `getpeereid()` for cred.
 *   - **Windows:** ⛔ `#error` — design plan documented in the .cpp;
 *     implementation tracked under task **#261**.  Different IPC
 *     model entirely: named pipes instead of Unix sockets;
 *     `CreateFileMapping` instead of `memfd_create`; `DuplicateHandle`
 *     instead of `SCM_RIGHTS`.  The `AcceptedPeer` struct uses a
 *     Windows-specific variant (pipe HANDLE + peer PID; no uid/gid
 *     because Windows has no such concept).
 *
 * @see docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md §5-§6, §9 D1+D4, §10.
 * @see docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md §1
 *      Amendment 2026-06-16 (the supersession that motivates this module).
 */

#include "plh_platform.hpp"  // PYLABHUB_IS_WINDOWS / PYLABHUB_IS_POSIX; pulls in <windows.h> on Win64
#include "pylabhub_utils_export.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#if defined(PYLABHUB_IS_POSIX)
#    include <sys/types.h>  // pid_t, uid_t, gid_t — POSIX peer-cred triple
#endif

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
    /// **Ownership.**  The caller owns the peer-side resource handle
    /// (`peer_socket_fd` on POSIX, `peer_pipe_handle` on Windows) —
    /// the producer does NOT track or close it.  The caller closes
    /// the handle after `send_capability()` (or after deciding to
    /// deny).  Letting the producer track accepted peer handles
    /// would create a recycle hazard if the caller closes early and
    /// the kernel reassigns the number before the producer's dtor
    /// runs.
    ///
    /// **POSIX shape** — `peer_socket_fd` is the accepted Unix-socket
    /// connection.  `pid` / `uid` / `gid` come from `SO_PEERCRED`
    /// (Linux), `getpeereid()` (FreeBSD / macOS), or their per-
    /// platform equivalent.  Used by L2's defence-in-depth sanity
    /// check (HEP-0041 §9 D4 step 3): the peer must be in the
    /// expected trust domain.  Zero defaults (when the cred read
    /// fails) fail closed under any non-root expectation.  On
    /// FreeBSD `pid` is always 0 because `xucred` doesn't carry it.
    ///
    /// **Windows shape** — `peer_pipe_handle` is a `HANDLE` to the
    /// connected named pipe (kept opaque as `void*` so the public
    /// header doesn't drag in `<windows.h>`).  `peer_pid` comes from
    /// `GetNamedPipeClientProcessId`; the producer uses it later to
    /// `OpenProcess(PROCESS_DUP_HANDLE)` + `DuplicateHandle` the
    /// SHM section into the peer.  No `uid`/`gid` fields — Windows
    /// has no such concept; L2 uses different attestation
    /// (process-token introspection, deferred).
    struct AcceptedPeer
    {
#if defined(PYLABHUB_IS_WINDOWS)
        // void* opaque follows the same convention as
        // `pylabhub::platform::ShmHandle::opaque` (plh_platform.hpp:152):
        // public structs do not drag <windows.h> through; the .cpp
        // casts back to `HANDLE` internally.  Caller must `CloseHandle`
        // to release.
        void          *peer_pipe_handle{nullptr};
        unsigned long  peer_pid{0};
#else
        int   peer_socket_fd{-1};
        pid_t pid{};
        uid_t uid{};
        gid_t gid{};
#endif
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
    /// Producer retains its own copy — the kernel duplicates the
    /// descriptor / HANDLE into the peer's process during the send.
    /// Returns true on successful send.  Caller is responsible for
    /// any surrounding auth flow (signed-nonce verify, broker
    /// pre-confirm) — this method is the unconditional handoff once
    /// the auth layer says "send it."
    ///
    /// **The peer-handle remains caller-owned by this call.**  See
    /// the `AcceptedPeer` docstring.  This method neither closes
    /// the peer handle on success nor on failure — failure paths
    /// leave it open so the caller may inspect errno, log peer
    /// details, or retry.  Caller must close after the handoff (or
    /// after deciding to abandon it).
    ///
    /// **Per-platform signature.**  On POSIX, takes the accepted
    /// socket fd (int).  On Windows, takes the connected pipe HANDLE
    /// (opaque void*) plus the peer's PID (the latter is required
    /// because `DuplicateHandle` needs an open `OpenProcess` handle
    /// to the target, not the pipe).
#if defined(PYLABHUB_IS_WINDOWS)
    virtual bool send_capability(void          *peer_pipe_handle,
                                 unsigned long  peer_pid) = 0;
#else
    virtual bool send_capability(int peer_socket_fd) = 0;
#endif

    /// Producer-side RW view of the anonymous SHM segment.  Span
    /// is stable across the producer's lifetime; bytes are visible
    /// to the consumer after `send_capability()` succeeds.
    [[nodiscard]] virtual std::span<std::byte> data() = 0;

    /// Segment size in bytes, fixed at construction.
    [[nodiscard]] virtual size_t size() const noexcept = 0;

    /// POSIX-only: borrow the underlying anonymous-SHM fd for an
    /// independent `mmap` (e.g. DataBlock fd-based attach in substep
    /// 1f).  The returned fd remains owned by this producer instance
    /// — caller MUST NOT close it.  Caller should `dup()` if they
    /// want an fd that outlives this producer.  Returns -1 if the
    /// producer is not yet constructed (no anonymous SHM allocated).
    ///
    /// Not exposed on Windows: the analogous facility there is the
    /// pre-existing pagefile HANDLE which would need a different
    /// abstraction (Windows mapping is `MapViewOfFile`, not `mmap`).
    /// Cross-platform DataBlock integration is a separate concern
    /// tracked under #261 + a sibling task.
#if !defined(PYLABHUB_IS_WINDOWS)
    [[nodiscard]] virtual int borrow_fd() const noexcept = 0;
#endif

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

    /// POSIX-only: borrow the underlying received fd.  See the matching
    /// `IShmCapabilityProducer::borrow_fd` docstring for the same
    /// ownership + lifetime + Windows-omission rationale.
#if !defined(PYLABHUB_IS_WINDOWS)
    [[nodiscard]] virtual int borrow_fd() const noexcept = 0;
#endif

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

/// HEP-CORE-0041 1i-mig-4 (#272) — recv the SHM fd via `SCM_RIGHTS`
/// on an ALREADY-CONNECTED + POST-§5.5-HANDSHAKE socket fd.  Takes
/// ownership of `socket_fd`: closes it before return on every path
/// (success, timeout, recv failure, exception).  Returns a fresh
/// `IShmCapabilityConsumer` owning the received memfd + the mmap.
///
/// Used by `RoleAPIBase::apply_consumer_reg_ack` SHM branch — the
/// consumer dial sequence is:
///   1. `initiate_consumer_handshake(endpoint, auth_material,
///       producer_pubkey, timeout)` returns the connected socket fd
///       (post-handshake; consumer has sent Frame 2; producer has
///       verified the challenge-response).
///   2. **THIS HELPER** consumes that fd: polls for the SCM_RIGHTS
///       message, recvmsg's the memfd, mmaps it, returns the
///       consumer instance.
///
/// Separation from `attach_shm_capability_consumer(endpoint)` (which
/// bundles connect + recv) lets the dial sequence interleave the
/// §5.5 handshake between connect and recv — required for the
/// HEP-CORE-0041 capability transport flow.  The bundled-connect
/// variant remains useful for tests and the pre-§5.5 paths that
/// don't need handshake (e.g. orchestrator L2 tests with a synthetic
/// no-auth producer).
///
/// Throws `std::runtime_error` on poll/recvmsg failure or on
/// timeout.  Throws `std::runtime_error` on non-Linux platforms
/// until the per-platform backends (#259/#260/#261) ship.
PYLABHUB_UTILS_EXPORT std::unique_ptr<IShmCapabilityConsumer>
attach_shm_capability_consumer_from_socket(
    int                       socket_fd,
    std::chrono::milliseconds timeout);

/// HEP-CORE-0041 substep 1g (#254) — compute the canonical per-user
/// runtime endpoint string for a SHM channel's capability transport.
///
/// The producer side publishes this string in REG_REQ
/// (`shm_capability_endpoint` field, HEP-0041 §5.1); the broker echoes
/// it back to authorized consumers in CONSUMER_REG_ACK (§5.3); the
/// consumer eventually passes it to `attach_shm_capability_consumer`
/// to dial the producer's L2 attach listener.
///
/// The returned string includes the `unix://` scheme prefix on POSIX
/// (mirroring ZMQ's `tcp://` / `ipc://` shape).  Callers feeding the
/// path to the L1 backend's `bind_endpoint` / `attach_shm_capability_consumer`
/// strip the scheme via `strip_unix_scheme` (or equivalent) before
/// the kernel `bind`/`connect` call.
///
/// Per-platform mapping:
/// - **Linux**: `unix://${XDG_RUNTIME_DIR}/pylabhub/shmcap-<channel>.sock`
///   when `XDG_RUNTIME_DIR` is set (kernel-enforced mode 0700 per-user
///   isolation from systemd `pam_systemd.so`); otherwise falls back to
///   `unix:///tmp/pylabhub-shmcap-<uid>-<channel>.sock` with a startup
///   WARN that the deployment is on a non-systemd host (uid embedded
///   in the filename for cross-user collision avoidance in the
///   world-writable `/tmp`).  Caller is responsible for `mkdir -p`'ing
///   the `pylabhub/` subdir at mode 0700 before bind (in the XDG case;
///   the `/tmp` fallback writes directly).
/// - **FreeBSD/macOS/Windows**: not yet implemented (tasks #259/#260/
///   #261).  See per-platform plans in shm_capability_channel.cpp.
///
/// Throws `std::runtime_error` on platforms whose backend hasn't
/// landed.  Throws `std::invalid_argument` on empty `channel_name`.
///
/// @param channel_name  HEP-0021 channel name; must be non-empty and
///                      pass the broker's channel-name validation
///                      (printable ASCII, no `/`, no scheme separators).
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string
default_shm_capability_endpoint(std::string_view channel_name);

/// HEP-CORE-0041 §5.1 — strip the canonical `unix://` URI scheme prefix
/// from a capability endpoint string, returning the bare filesystem
/// path suitable for `bind(2)` / `connect(2)`.  If no scheme is
/// present, the input is returned unchanged (existing L2 tests +
/// advanced callers may pass bare paths).
///
/// The producer side emits the URI shape via
/// `default_shm_capability_endpoint` and publishes it on the wire
/// (REG_REQ.shm_capability_endpoint, echoed to consumers in
/// CONSUMER_REG_ACK).  The L1 backend's `bind_endpoint` /
/// `attach_shm_capability_consumer` strip the scheme before the
/// kernel syscall — `sockaddr_un::sun_path` is the bare path.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string
strip_unix_scheme(std::string_view endpoint);

} // namespace pylabhub::utils::security
