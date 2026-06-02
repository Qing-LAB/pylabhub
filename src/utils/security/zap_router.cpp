/**
 * @file zap_router.cpp
 * @brief Implementation of ZapRouter — caller-pumped ZAP handler.
 *        PeerAdmission Phase C.
 *
 * No internal thread.  No ThreadManager.  No re-entrancy with
 * LifecycleManager (avoids the deadlock recorded in design §12.5
 * S13).  Startup binds an inproc REP socket; shutdown closes it.
 * Callers pump via `pump_one(timeout)`.
 *
 * Dependencies (LifecycleManager edges):
 *   - `pylabhub::utils::Logger`   — for LOG_INFO/WARN/ERROR
 *   - `ZMQContext`                — for the shared `zmq::context_t`
 */
#include "utils/security/zap_router.hpp"

#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/module_def.hpp"
#include "utils/zmq_context.hpp"
#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"   // recv_multipart

#include <zmq.h>  // zmq_z85_encode

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pylabhub::utils::security
{

namespace
{

constexpr const char *kZapModuleName       = "ZapRouter";
constexpr const char *kZapInprocEndpoint   = "inproc://zeromq.zap.01";
constexpr const char *kZapVersion          = "1.0";
constexpr std::size_t kCurvePubkeyRawBytes = 32;
constexpr std::size_t kCurvePubkeyZ85Chars = 40;

// ZAP request frame layout (RFC 27 / ZMQ 4.x):
//   [0] version       ("1.0")
//   [1] request_id    (echoed in reply)
//   [2] domain        (matches socket's `zap_domain` sockopt)
//   [3] address       (peer IP, ignored)
//   [4] identity      (peer ZMQ identity, ignored)
//   [5] mechanism     ("CURVE")
//   [6] credentials   (32 raw bytes for CURVE: peer's public key)
constexpr std::size_t kZapMinReqFrames     = 7;
constexpr std::size_t kZapFrameRequestId   = 1;
constexpr std::size_t kZapFrameDomain      = 2;
constexpr std::size_t kZapFrameMechanism   = 5;
constexpr std::size_t kZapFrameCredentials = 6;

/// Z85-encode a raw 32-byte CURVE pubkey to its 40-char canonical
/// representation.  Returns empty on size mismatch.
std::string z85_encode_pubkey(const void *src, std::size_t len)
{
    if (len != kCurvePubkeyRawBytes)
        return {};
    std::array<char, kCurvePubkeyZ85Chars + 1> buf{};
    if (zmq_z85_encode(buf.data(),
                        static_cast<const std::uint8_t *>(src),
                        len) == nullptr)
        return {};
    return std::string(buf.data(), kCurvePubkeyZ85Chars);
}

/// Send a ZAP reply per RFC 27:
///   [0] version, [1] request_id, [2] status_code,
///   [3] status_text, [4] user_id, [5] metadata (empty).
void send_zap_reply(zmq::socket_t      &sock,
                    const std::string  &request_id,
                    const char         *status_code,
                    const char         *status_text,
                    const std::string  &user_id)
{
    std::array<zmq::const_buffer, 6> frames{
        zmq::buffer(kZapVersion, std::strlen(kZapVersion)),
        zmq::buffer(request_id.data(), request_id.size()),
        zmq::buffer(status_code, std::strlen(status_code)),
        zmq::buffer(status_text, std::strlen(status_text)),
        zmq::buffer(user_id.data(), user_id.size()),
        zmq::buffer("", 0),
    };
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        const auto flags = (i + 1 < frames.size())
                               ? zmq::send_flags::sndmore
                               : zmq::send_flags::none;
        (void) sock.send(frames[i], flags);
    }
}

} // namespace

// ── ZapRouter::Impl ─────────────────────────────────────────────────────────

struct ZapRouter::Impl
{
    /// Routing map: zap_domain → PeerAdmission*.  Raw pointer; the
    /// `ZapDomainHandle` returned by `register_domain` keeps the
    /// mapping alive only for as long as the queue (or other
    /// admission-bearing surface) needs it.  The handle's destructor
    /// removes the map entry under `registered_mu` BEFORE the queue
    /// destructs.
    std::unordered_map<std::string, PeerAdmission *> registered;

    /// Many readers (`pump_one` looking up admission by domain),
    /// rare writers (register/unregister).
    mutable std::shared_mutex registered_mu;

    /// Inproc REP socket.  Created by `on_module_startup_`, destroyed
    /// by `on_module_shutdown_`.  After startup, accessed ONLY by
    /// the single pumping thread per the contract documented in the
    /// header — no internal sync needed.
    std::optional<zmq::socket_t> sock;

    /// Whether `register_dynamic_module` has been called this
    /// process-life-cycle.  Set on first `register_domain`; cleared
    /// at module shutdown so re-init after a finalize cycle works
    /// in test harnesses.
    std::atomic<bool> module_registered{false};

    /// Serializes module-registration + sock create/destruct against
    /// concurrent `register_domain` calls.
    std::mutex lifecycle_mu;

    /// Lifetime-cumulative ZAP-reply counters.  See zap_router.hpp
    /// docstrings.  Incremented in `pump_one` exactly once per reply
    /// sent.  Reset to 0 at module shutdown so re-init within one
    /// process (test harnesses with finalize+reinit cycles) starts
    /// from a known state.
    std::atomic<std::uint64_t> allowed_count{0};
    std::atomic<std::uint64_t> denied_count{0};
};

ZapRouter::ZapRouter() : impl_(std::make_unique<Impl>()) {}
ZapRouter::~ZapRouter() = default;

ZapRouter &ZapRouter::instance()
{
    static ZapRouter inst;
    return inst;
}

const char *ZapRouter::module_name() noexcept { return kZapModuleName; }

// ── Lifecycle thunks → instance methods ─────────────────────────────────────

void ZapRouter::lifecycle_startup_thunk(const char * /*name*/,
                                          void * /*userdata*/)
{
    instance().on_module_startup_();
}

void ZapRouter::lifecycle_shutdown_thunk(const char * /*name*/,
                                           void * /*userdata*/)
{
    instance().on_module_shutdown_();
}

void ZapRouter::on_module_startup_()
{
    std::lock_guard<std::mutex> lk(impl_->lifecycle_mu);
    if (impl_->sock.has_value())
        return;  // idempotent — repeated LoadModule under persistent flag

    try
    {
        impl_->sock.emplace(pylabhub::hub::get_zmq_context(),
                             zmq::socket_type::rep);
        impl_->sock->set(zmq::sockopt::linger, 0);
        impl_->sock->bind(kZapInprocEndpoint);
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("ZapRouter: bind('{}') failed: {} — ZAP "
                     "authentication WILL NOT WORK in this process "
                     "(libzmq sockets that expected ZAP will hang on "
                     "handshake)", kZapInprocEndpoint, e.what());
        impl_->sock.reset();
        return;
    }
    LOGGER_INFO("ZapRouter: bound REP socket at '{}' — call pump_one() "
                "from your event loop to service CURVE handshakes",
                kZapInprocEndpoint);
}

void ZapRouter::on_module_shutdown_()
{
    std::lock_guard<std::mutex> lk(impl_->lifecycle_mu);
    if (impl_->sock.has_value())
    {
        impl_->sock.reset();
        LOGGER_INFO("ZapRouter: REP socket closed");
    }
    // Reset observability counters so a finalize+reinit cycle within
    // one process (test harnesses) starts from zero.
    impl_->allowed_count.store(0, std::memory_order_relaxed);
    impl_->denied_count.store(0, std::memory_order_relaxed);
    // Allow re-registration in a future LifecycleGuard cycle (test
    // harnesses that finalize + re-initialize within one process).
    impl_->module_registered.store(false, std::memory_order_release);
}

pylabhub::utils::ModuleDef ZapRouter::make_module_def_()
{
    pylabhub::utils::ModuleDef def(kZapModuleName);
    def.add_dependency("pylabhub::utils::Logger");
    def.add_dependency("ZMQContext");
    def.set_startup(ZapRouter::lifecycle_startup_thunk);
    def.set_shutdown(ZapRouter::lifecycle_shutdown_thunk,
                     std::chrono::milliseconds(500));
    def.set_as_persistent(true);
    return def;
}

// ── register_domain / unregister_domain_ ────────────────────────────────────

ZapDomainHandle
ZapRouter::register_domain(std::string domain, PeerAdmission *admission)
{
    if (domain.empty())
        throw std::runtime_error(
            "ZapRouter::register_domain: domain must be non-empty");
    if (admission == nullptr)
        throw std::runtime_error(
            "ZapRouter::register_domain: admission must be non-null "
            "(domain='" + domain + "')");

    // First-ever caller in the process registers the dynamic module.
    // Subsequent callers just LoadModule (ref-count++).
    {
        std::lock_guard<std::mutex> lk(impl_->lifecycle_mu);
        if (!impl_->module_registered.load(std::memory_order_acquire))
        {
            if (!pylabhub::utils::LifecycleManager::instance()
                    .register_dynamic_module(make_module_def_()))
            {
                LOGGER_WARN(
                    "ZapRouter: register_dynamic_module returned false "
                    "(domain='{}') — module may already be registered "
                    "in this lifecycle cycle", domain);
            }
            impl_->module_registered.store(true,
                                            std::memory_order_release);
        }
    }

    // LoadModule both bumps ref-count AND triggers startup the first
    // time.  Persistent flag means UnloadModule that drops ref_count
    // to zero does NOT trigger shutdown.
    if (!pylabhub::utils::LoadModule(kZapModuleName))
        throw std::runtime_error(
            "ZapRouter::register_domain: LoadModule('" +
            std::string(kZapModuleName) + "') failed (domain='" +
            domain + "')");

    {
        std::unique_lock<std::shared_mutex> lk(impl_->registered_mu);
        if (auto it = impl_->registered.find(domain);
            it != impl_->registered.end())
        {
            // Roll back the LoadModule to keep ref-counts accurate.
            (void) pylabhub::utils::UnloadModule(kZapModuleName);
            throw std::runtime_error(
                "ZapRouter::register_domain: domain '" + domain +
                "' is already registered (double-registration is a "
                "regression — each zap_domain must be unique)");
        }
        impl_->registered.emplace(domain, admission);
    }

    return ZapDomainHandle(this, std::move(domain));
}

void ZapRouter::unregister_domain_(const std::string &domain)
{
    if (domain.empty()) return;
    {
        std::unique_lock<std::shared_mutex> lk(impl_->registered_mu);
        impl_->registered.erase(domain);
    }
    (void) pylabhub::utils::UnloadModule(kZapModuleName);
}

std::size_t ZapRouter::registered_domain_count_for_test() const
{
    std::shared_lock<std::shared_mutex> lk(impl_->registered_mu);
    return impl_->registered.size();
}

std::uint64_t ZapRouter::allowed_count() const noexcept
{
    return impl_->allowed_count.load(std::memory_order_relaxed);
}

std::uint64_t ZapRouter::denied_count() const noexcept
{
    return impl_->denied_count.load(std::memory_order_relaxed);
}

// ── pump_one ────────────────────────────────────────────────────────────────

bool ZapRouter::pump_one(std::chrono::milliseconds timeout)
{
    // Snapshot the socket pointer.  If the module isn't loaded yet
    // (no register_domain called), nothing to do.  Touching
    // impl_->sock without lifecycle_mu is safe because the pumping
    // thread observes the load via happens-before from the
    // register_domain that triggered LoadModule (which completed
    // before the pumper started).
    if (!impl_->sock.has_value())
        return false;

    zmq::socket_t &sock = *impl_->sock;
    sock.set(zmq::sockopt::rcvtimeo,
              static_cast<int>(timeout.count()));

    std::vector<zmq::message_t> req;
    zmq::recv_result_t          rr;
    try
    {
        rr = zmq::recv_multipart(sock, std::back_inserter(req),
                                  zmq::recv_flags::none);
    }
    catch (const zmq::error_t &e)
    {
        if (e.num() == ETERM || e.num() == EINTR)
            return false;
        LOGGER_WARN("ZapRouter::pump_one: recv error: {}", e.what());
        return false;
    }
    if (!rr.has_value())
        return false;  // RCVTIMEO

    // Empty multipart cannot happen on a healthy REP socket (recv would
    // have failed), but defensively: if it did, we can't construct a
    // reply (no request_id to echo) and the socket is wedged.  Log and
    // bail; the next pump cycle will surface EFSM on recv.
    if (req.empty())
    {
        LOGGER_ERROR("ZapRouter::pump_one: recv_multipart returned 0 "
                     "frames — REP socket FSM is wedged.  ZAP will not "
                     "serve further requests until the module reloads");
        impl_->denied_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // **C-Z1 fix.**  A truncated request (1..6 frames) must still
    // receive a reply, otherwise the REP socket is left in the SEND
    // state and the next recv throws EFSM — wedging ZAP for the
    // entire process.  Echo whichever frames we have; pin a "400
    // Malformed request" status so the client (libzmq or otherwise)
    // surfaces a coherent error.
    if (req.size() < kZapMinReqFrames)
    {
        const std::string request_id =
            (req.size() > kZapFrameRequestId)
                ? req[kZapFrameRequestId].to_string()
                : std::string{};
        LOGGER_WARN("ZapRouter::pump_one: malformed ZAP request "
                    "(frame_count={}) — replying 400 to keep REP "
                    "socket in valid FSM state", req.size());
        send_zap_reply(sock, request_id, "400",
                       "Malformed request", "");
        impl_->denied_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // **H-Z1 fix.**  RFC 27 reserves frame [0] for the protocol
    // version.  Anything other than "1.0" is by definition outside
    // the protocol we implement; surface a deterministic 400 rather
    // than reading downstream frames under unknown semantics.
    const std::string version    = req[0].to_string();
    const std::string request_id = req[kZapFrameRequestId].to_string();
    if (version != kZapVersion)
    {
        LOGGER_WARN("ZapRouter::pump_one: unsupported ZAP version "
                    "'{}' (expected '{}') — rejecting", version,
                    kZapVersion);
        send_zap_reply(sock, request_id, "400", "Bad version", "");
        impl_->denied_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const std::string domain    = req[kZapFrameDomain].to_string();
    const std::string mechanism = req[kZapFrameMechanism].to_string();

    if (mechanism != "CURVE")
    {
        LOGGER_WARN("ZapRouter::pump_one: rejecting non-CURVE "
                    "mechanism '{}' for domain '{}'", mechanism, domain);
        send_zap_reply(sock, request_id, "400",
                       "Unsupported mechanism", "");
        impl_->denied_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const auto       &cred = req[kZapFrameCredentials];
    const std::string z85  =
        z85_encode_pubkey(cred.data(), cred.size());
    if (z85.empty())
    {
        LOGGER_WARN("ZapRouter::pump_one: rejecting handshake for "
                    "domain '{}' — credentials size {} != {} (CURVE "
                    "pubkey)", domain, cred.size(), kCurvePubkeyRawBytes);
        send_zap_reply(sock, request_id, "400",
                       "Bad credentials", "");
        impl_->denied_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    PeerAdmission *admission = nullptr;
    {
        std::shared_lock<std::shared_mutex> lk(impl_->registered_mu);
        if (auto it = impl_->registered.find(domain);
            it != impl_->registered.end())
            admission = it->second;
    }
    if (admission == nullptr)
    {
        LOGGER_WARN("ZapRouter::pump_one: rejecting handshake — no "
                    "domain registered for '{}' (either misconfigured "
                    "zap_domain or unsolicited probe)", domain);
        send_zap_reply(sock, request_id, "400",
                       "Domain not registered", "");
        impl_->denied_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const bool ok = admission->is_peer_allowed(
        PeerIdentity{"curve", z85});
    if (ok)
    {
        send_zap_reply(sock, request_id, "200", "OK", z85);
        impl_->allowed_count.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        send_zap_reply(sock, request_id, "400",
                       "Not in allowlist", "");
        impl_->denied_count.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

// ── ZapDomainHandle ─────────────────────────────────────────────────────────

ZapDomainHandle::ZapDomainHandle(ZapDomainHandle &&other) noexcept
    : router_(other.router_), domain_(std::move(other.domain_))
{
    other.router_ = nullptr;
    other.domain_.clear();
}

ZapDomainHandle &
ZapDomainHandle::operator=(ZapDomainHandle &&other) noexcept
{
    if (this == &other) return *this;
    if (is_active())
        router_->unregister_domain_(domain_);
    router_       = other.router_;
    domain_       = std::move(other.domain_);
    other.router_ = nullptr;
    other.domain_.clear();
    return *this;
}

ZapDomainHandle::~ZapDomainHandle()
{
    if (is_active())
        router_->unregister_domain_(domain_);
}

// ── ZapPumpThread ───────────────────────────────────────────────────────────

struct ZapPumpThread::Impl
{
    std::atomic<bool> stop{false};
    std::thread       thread;
};

ZapPumpThread::ZapPumpThread(std::chrono::milliseconds tick)
    : impl_(std::make_unique<Impl>())
{
    auto *impl_ptr = impl_.get();
    impl_->thread  = std::thread([impl_ptr, tick]() {
        while (!impl_ptr->stop.load(std::memory_order_acquire))
        {
            // pump_one returns false on timeout OR module-not-loaded.
            // Either way we just spin to the next tick to check stop.
            (void) ZapRouter::instance().pump_one(tick);
        }
    });
}

ZapPumpThread::~ZapPumpThread()
{
    if (!impl_) return;
    impl_->stop.store(true, std::memory_order_release);
    if (impl_->thread.joinable())
        impl_->thread.join();
}

} // namespace pylabhub::utils::security
