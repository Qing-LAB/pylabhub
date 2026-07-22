#pragma once
/**
 * @file attach_channel_shm.hpp
 * @brief SHM/AF_UNIX binding of `IAttachChannel` — the transport
 *        adapter used by `AttachProtocolAcceptor` and the SHM
 *        consumer-side handshake.
 *
 * @author  pyLabHub team
 * @date    2026-07-07 (Phase 3 — extract from `attach_protocol.cpp`)
 * @copyright  MIT
 * @see  attach_channel.hpp  (interface + design rationale)
 * @see  HEP-CORE-0041 §9 D4  (SHM producer-side handshake)
 *
 * # Wire format
 *
 * Each JSON frame is transmitted as:
 * ```
 * [ 4-byte little-endian length ] [ length bytes of JSON body ]
 * ```
 *
 * A 4 KiB (`kMaxAttachFrameBytes`) DoS cap applies to the JSON body
 * on both send and recv paths; oversized frames throw immediately
 * without touching the wire.
 *
 * # Deadline discipline
 *
 * Both `send_frame` and `recv_frame` take an absolute
 * `steady_clock::time_point`.  The SHM implementation uses
 * `poll(POLLOUT)` / `poll(POLLIN)` with remaining budget before each
 * `send`/`recv` syscall — a stalled peer that never reads / never
 * writes cannot burn more than the caller's declared budget total
 * across the whole handshake (see #318 / #319 for the fd-level
 * rationale).
 *
 * # Ownership
 *
 * `ShmAttachChannel` does NOT own the fd it wraps.  The caller
 * (typically `AttachProtocolAcceptor::accept_one` or the consumer
 * handshake) retains ownership via an `FdGuard` RAII wrapper and
 * releases it on success (SCM_RIGHTS handover follows) or closes it
 * on any error path.  The channel's dtor is a no-op.
 *
 * This split (channel != fd owner) matches the SHM AttachProtocol's
 * fd-handoff semantic — after the handshake, the SAME fd is used for
 * `recvmsg(SCM_RIGHTS)` to receive the memfd capability.  Making the
 * channel own the fd would force a `.release()` dance across the
 * handshake boundary; keeping the channel non-owning is simpler.
 */

#include "plh_platform.hpp"
#include "pylabhub_utils_export.h"
#include "utils/security/attach_channel.hpp"

#include <chrono>
#include <string>
#include <utility>

#if defined(PYLABHUB_PLATFORM_LINUX)

namespace pylabhub::utils::security
{

/// SHM/AF_UNIX binding of `IAttachChannel`.  Length-prefixed JSON
/// over a `SOCK_STREAM` file descriptor.  See file docstring for the
/// wire format, deadline discipline, and fd-ownership contract.
class PYLABHUB_UTILS_EXPORT ShmAttachChannel final : public IAttachChannel
{
  public:
    /// Construct a channel bound to `fd`.  The channel does NOT
    /// take ownership; the caller retains close responsibility.
    ///
    /// @param fd    An open AF_UNIX SOCK_STREAM file descriptor.
    ///              Must remain valid for the lifetime of the channel.
    /// @param side  Diagnostic tag inserted into error messages
    ///              ("producer" / "consumer").  Not part of the wire
    ///              protocol.  Stored by copy — safe to pass a
    ///              temporary string.
    ShmAttachChannel(int fd, std::string side) : fd_(fd), side_(std::move(side)) {}

    ~ShmAttachChannel() override = default;

    ShmAttachChannel(const ShmAttachChannel &) = delete;
    ShmAttachChannel &operator=(const ShmAttachChannel &) = delete;
    ShmAttachChannel(ShmAttachChannel &&) = delete;
    ShmAttachChannel &operator=(ShmAttachChannel &&) = delete;

    void send_frame(const nlohmann::json &frame,
                    std::chrono::steady_clock::time_point deadline) override;

    nlohmann::json recv_frame(std::chrono::steady_clock::time_point deadline) override;

    /// The underlying fd (non-owning view).  Exposed only for the
    /// caller's post-handshake SCM_RIGHTS handoff — no code path
    /// outside `attach_protocol.cpp` / `shm_attach_orchestrator.cpp`
    /// should touch it.
    [[nodiscard]] int fd() const noexcept { return fd_; }

  private:
    int fd_;
    std::string side_;
};

} // namespace pylabhub::utils::security

#endif // PYLABHUB_PLATFORM_LINUX
