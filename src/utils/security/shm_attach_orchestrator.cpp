/**
 * @file shm_attach_orchestrator.cpp
 * @brief Linux implementation of substep 1e (HEP-CORE-0041 §9 D4
 *        steps 6-7).  See shm_attach_orchestrator.hpp for the design.
 */
#include "utils/security/shm_attach_orchestrator.hpp"

#include "utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

#if defined(PYLABHUB_PLATFORM_LINUX)
#    include <unistd.h>
#endif

namespace pylabhub::utils::security
{

#if defined(PYLABHUB_PLATFORM_LINUX)

namespace
{

[[maybe_unused]] void
close_if_valid(int fd) noexcept
{
    if (fd >= 0)
        ::close(fd);
}

} // anonymous namespace

ShmAttachOrchestrator::ShmAttachOrchestrator(
    AttachProtocolAcceptor &acceptor, IShmCapabilityProducer &transport,
    Config config)
    : acceptor_(acceptor),
      transport_(transport),
      config_(std::move(config))
{
    if (config_.channel_name.empty())
    {
        throw std::invalid_argument(
            "ShmAttachOrchestrator: channel_name must be non-empty");
    }
    if (config_.producer_role_uid.empty())
    {
        throw std::invalid_argument(
            "ShmAttachOrchestrator: producer_role_uid must be non-empty");
    }
    if (!config_.cache_lookup)
    {
        throw std::invalid_argument(
            "ShmAttachOrchestrator: cache_lookup callback must be set");
    }
    if (!config_.broker_query)
    {
        throw std::invalid_argument(
            "ShmAttachOrchestrator: broker_query callback must be set");
    }
}

ShmAttachOrchestrator::Outcome
ShmAttachOrchestrator::accept_and_serve_one(std::chrono::milliseconds timeout)
{
    // ── Step 1: accept + L2 handshake ─────────────────────────────────────
    //
    // AttachProtocolAcceptor::accept_one() closes the peer fd on any
    // failure path before throwing, so we don't have an fd to clean up
    // in the catch handler.  Success path hands ownership of the fd
    // back to us via AuthenticatedConsumer.raw_peer.peer_socket_fd.
    std::optional<AuthenticatedConsumer> auth_opt;
    try
    {
        auth_opt = acceptor_.accept_one(timeout);
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN(
            "[shm_attach] handshake failed channel='{}': {}",
            config_.channel_name, e.what());
        return Outcome::HandshakeFailed;
    }
    if (!auth_opt.has_value())
        return Outcome::Timeout;

    AuthenticatedConsumer &auth    = *auth_opt;
    const int              peer_fd = auth.raw_peer.peer_socket_fd;
    if (peer_fd < 0)
    {
        LOGGER_WARN(
            "[shm_attach] accept returned valid AuthenticatedConsumer "
            "with invalid peer_socket_fd channel='{}'",
            config_.channel_name);
        return Outcome::HandshakeFailed;
    }

    // RAII guard: closes fd on any non-Sent return path.  The send
    // path RELEASES the guard after a successful send_capability so
    // the caller (consumer) is the one holding the duplicated fd
    // from then on; we still own our local end and close it.
    struct FdGuard
    {
        int  fd;
        bool released{false};
        ~FdGuard() { if (!released && fd >= 0) ::close(fd); }
    };
    FdGuard guard{peer_fd};

    // ── Step 2: broker pre-confirm ────────────────────────────────────────
    //
    // Per HEP-0041 §9 D4: producer sends CONSUMER_ATTACH_REQ_SHM to broker
    // and reads the reply.  Reply shape (from substep 1d handler):
    //   { status: "success" | "denied", denial_reason?: ... }
    // Or the BrokerQuery callback returns nullopt on transport-level
    // failure (timeout, broker unreachable).  We fail closed on
    // nullopt — broker authority cannot be ascertained, so admission
    // is not safe.
    std::optional<nlohmann::json> broker_reply_opt;
    try
    {
        broker_reply_opt =
            config_.broker_query(auth.consumer_pubkey_z85, auth.consumer_role_uid);
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN(
            "[shm_attach] broker_query callback threw "
            "channel='{}' consumer_pubkey='{}' consumer_uid='{}': {} "
            "— failing closed",
            config_.channel_name, auth.consumer_pubkey_z85,
            auth.consumer_role_uid, e.what());
        return Outcome::DeniedTransportFail;
    }
    if (!broker_reply_opt.has_value())
    {
        LOGGER_WARN(
            "[shm_attach] broker_query returned nullopt "
            "channel='{}' consumer_pubkey='{}' consumer_uid='{}' "
            "— failing closed (broker authority cannot be ascertained)",
            config_.channel_name, auth.consumer_pubkey_z85,
            auth.consumer_role_uid);
        return Outcome::DeniedTransportFail;
    }

    const nlohmann::json &broker_reply = *broker_reply_opt;
    const std::string     status =
        broker_reply.value("status", std::string{});
    const bool broker_says_allowed = (status == "success");
    const bool broker_says_denied  = (status == "denied");
    if (!broker_says_allowed && !broker_says_denied)
    {
        // Anything other than success/denied is a protocol violation
        // (broker should reply with one of these or an ERROR frame,
        // which the BRC layer would have surfaced as nullopt).  Fail
        // closed.
        LOGGER_WARN(
            "[shm_attach] broker reply has unexpected status='{}' "
            "channel='{}' consumer_pubkey='{}' consumer_uid='{}' "
            "— failing closed",
            status, config_.channel_name, auth.consumer_pubkey_z85,
            auth.consumer_role_uid);
        return Outcome::DeniedTransportFail;
    }

    // ── Step 3: cache lookup + divergence-WARN table ─────────────────────
    //
    // HEP-0041 §9 D4 cached-allowlist semantics: broker wins, but
    // disagreement between cache and broker is observability for the
    // broker-NOTIFY pipeline health metric.
    bool cache_says_allowed = false;
    try
    {
        cache_says_allowed = config_.cache_lookup(auth.consumer_pubkey_z85);
    }
    catch (const std::exception &e)
    {
        // Cache lookup throwing is anomalous — log and treat as
        // cache=denied for the divergence computation.  Broker's
        // verdict still governs admission.
        LOGGER_WARN(
            "[shm_attach] cache_lookup callback threw "
            "channel='{}' consumer_pubkey='{}': {} — treating cache as denied",
            config_.channel_name, auth.consumer_pubkey_z85, e.what());
        cache_says_allowed = false;
    }

    if (broker_says_allowed && !cache_says_allowed)
    {
        LOGGER_WARN(
            "[shm_attach] divergence broker=allowed cache=denied "
            "channel='{}' consumer_pubkey='{}' consumer_uid='{}' "
            "— admitting (broker authoritative; producer missed a "
            "CHANNEL_AUTH_CHANGED_NOTIFY add or its pull failed)",
            config_.channel_name, auth.consumer_pubkey_z85,
            auth.consumer_role_uid);
    }
    else if (broker_says_denied && cache_says_allowed)
    {
        LOGGER_WARN(
            "[shm_attach] divergence broker=denied cache=allowed "
            "channel='{}' consumer_pubkey='{}' consumer_uid='{}' "
            "— denying (broker authoritative; producer missed a "
            "CHANNEL_AUTH_CHANGED_NOTIFY remove or its pull failed)",
            config_.channel_name, auth.consumer_pubkey_z85,
            auth.consumer_role_uid);
    }

    // ── Step 4: apply broker's verdict ────────────────────────────────────
    if (broker_says_allowed)
    {
        const bool sent = transport_.send_capability(peer_fd);
        if (!sent)
        {
            LOGGER_WARN(
                "[shm_attach] send_capability failed "
                "channel='{}' consumer_pubkey='{}' consumer_uid='{}' "
                "— failing closed",
                config_.channel_name, auth.consumer_pubkey_z85,
                auth.consumer_role_uid);
            return Outcome::DeniedTransportFail;
        }
        LOGGER_INFO(
            "[shm_attach] event=AttachAuthorized channel='{}' "
            "consumer_pubkey='{}' consumer_uid='{}'",
            config_.channel_name, auth.consumer_pubkey_z85,
            auth.consumer_role_uid);
        // The peer fd is still ours to close — `send_capability`
        // duplicates the SHM fd into the peer via SCM_RIGHTS but does
        // NOT close our peer socket fd (that's our local end of the
        // AF_UNIX connection used for the handshake).  Let the FdGuard
        // close it on return — handshake is done, we won't send
        // anything else to this consumer.
        return Outcome::Sent;
    }

    // Broker denied → log INFO and let the FdGuard drop the peer.
    LOGGER_INFO(
        "[shm_attach] event=AttachDenied channel='{}' "
        "consumer_pubkey='{}' consumer_uid='{}' denial_reason='{}'",
        config_.channel_name, auth.consumer_pubkey_z85,
        auth.consumer_role_uid,
        broker_reply.value("denial_reason", std::string{"unspecified"}));
    return Outcome::DeniedByBroker;
}

#endif // PYLABHUB_PLATFORM_LINUX

// HEP-CORE-0041 §6.5 + 1i-doc-sync #267: symmetric `#error` for
// non-Linux platforms.  L1 (shm_capability_channel.cpp) and L2b
// (attach_protocol.cpp) both fire `#error` on non-Linux to surface
// "not implemented" at compile time.  L2c (this file) was missing
// the same fallback — on a non-Linux build, the class would silently
// not exist and callers would hit "undefined symbol" link errors at
// use time.  The `#error` here surfaces the same "platform-not-yet-
// supported" diagnostic at compile time, matching L1+L2b discipline.
// Tasks #259 (FreeBSD), #260 (macOS), #261 (Windows) implement the
// per-platform L2c when they ship.
#if !defined(PYLABHUB_PLATFORM_LINUX)
#    if defined(PYLABHUB_PLATFORM_FREEBSD)
#        error "HEP-CORE-0041 §6.5: ShmAttachOrchestrator FreeBSD impl not yet shipped — task #259.  Builds against this platform must wait for the FreeBSD backend to land."
#    elif defined(PYLABHUB_PLATFORM_APPLE)
#        error "HEP-CORE-0041 §6.5: ShmAttachOrchestrator macOS impl not yet shipped — task #260.  Builds against this platform must wait for the macOS backend to land."
#    elif defined(PYLABHUB_PLATFORM_WIN64)
#        error "HEP-CORE-0041 §6.5: ShmAttachOrchestrator Windows impl not yet shipped — task #261.  Builds against this platform must wait for the Windows backend to land (named-pipe + DuplicateHandle)."
#    else
#        error "HEP-CORE-0041 §6.5: ShmAttachOrchestrator: unknown platform.  Phase 1 supports Linux only; FreeBSD/macOS/Windows are Phases 2-3."
#    endif
#endif

} // namespace pylabhub::utils::security
