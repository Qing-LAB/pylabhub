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
#include "utils/recursion_guard.hpp"  // RecursionGuard — Slice A (task #215)
#include "utils/zmq_context.hpp"
#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"   // recv_multipart

#include <zmq.h>  // zmq_z85_encode

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>  // std::exception for pump_one admission try/catch
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

    /// Slice A (task #215) — single-pumper invariant runtime enforcement.
    /// The libzmq REP socket FSM is single-thread; two threads inside
    /// pump_one simultaneously corrupt it.  `pump_one` increments
    /// on entry, decrements on exit, and PANICs if the post-increment
    /// value is > 1.  Detects concurrent pumpers loudly instead of
    /// silent FSM corruption.  See zap_router.hpp threading model.
    std::atomic<int> concurrent_pumpers{0};
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

    // Slice A (task #215) — reentrance refuse.  If this thread is
    // already inside `pump_one`'s admission decision (RecursionGuard
    // keyed on `this`), it holds `registered_mu` in shared mode;
    // attempting to acquire `registered_mu` in unique mode below
    // would be undefined behavior under std::shared_mutex.  Refuse
    // the call and return an inactive sentinel handle.  See
    // peer_admission.hpp `is_peer_allowed` reentrance contract.
    if (pylabhub::basics::RecursionGuard::is_recursing(this))
    {
        LOGGER_ERROR("ZapRouter::register_domain: reentrant call "
                     "detected from inside a PeerAdmission decision "
                     "(domain='{}').  Refusing — the call site is "
                     "violating the is_peer_allowed contract that "
                     "forbids registering domains during the admission "
                     "scope.  Returning inactive ZapDomainHandle.  "
                     "See peer_admission.hpp + HEP-CORE-0036 §7.",
                     domain);
        return ZapDomainHandle{};
    }

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

    // Slice A (task #215) — reentrance PANIC.  Unlike register_domain
    // (where refuse + inactive-handle is recoverable), an unregister
    // from inside the admission decision is unrecoverable: the
    // ZapDomainHandle whose destructor is running has no way to
    // observe the refusal, so the handle would be destroyed while the
    // router still holds a map entry pointing at admission/queue
    // memory that's about to be freed — a UAF on the next pump_one.
    // Loud failure is the only honest response.  See peer_admission.hpp
    // `is_peer_allowed` reentrance contract.
    if (pylabhub::basics::RecursionGuard::is_recursing(this))
    {
        PLH_PANIC("ZapRouter::unregister_domain_: reentrant call "
                  "detected from inside a PeerAdmission decision "
                  "(domain='{}').  This is unrecoverable — destroying "
                  "a ZapDomainHandle inside is_peer_allowed would "
                  "leave the router with a dangling map entry "
                  "pointing at memory that is about to be freed.  An "
                  "admission implementer's is_peer_allowed MUST NOT "
                  "trigger destruction of any ZapDomainHandle.  See "
                  "peer_admission.hpp + HEP-CORE-0036 §7.",
                  domain);
    }

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
    // Slice A (task #215) — single-pumper invariant runtime check.
    // libzmq's REP socket FSM is single-thread; concurrent pumpers
    // silently corrupt it.  Detect simultaneous entry loudly via the
    // atomic counter, PANIC instead of letting the FSM tear.  The
    // counter is incremented on entry and decremented on every exit
    // path (RAII scope guard via dtor).  See zap_router.hpp threading
    // model.
    struct PumpScope {
        std::atomic<int> &counter;
        bool entered;
        explicit PumpScope(std::atomic<int> &c)
            : counter(c), entered(false)
        {
            const int post = counter.fetch_add(1,
                std::memory_order_acq_rel) + 1;
            if (post > 1)
            {
                // Decrement before the PANIC so dtor doesn't double-
                // decrement a counter that may be inspected by an
                // unlikely-but-possible post-PANIC log handler.
                counter.fetch_sub(1, std::memory_order_release);
                PLH_PANIC("ZapRouter::pump_one: concurrent pumper "
                          "detected (post-increment count = {}).  The "
                          "libzmq REP socket FSM is single-thread; two "
                          "threads pumping simultaneously corrupt it.  "
                          "Production wires exactly ONE pumper (BRC "
                          "poll thread per HEP-CORE-0036 §7.1); tests "
                          "use ZapPumpThread (also one).  See "
                          "zap_router.hpp threading model.", post);
            }
            entered = true;
        }
        ~PumpScope() noexcept
        {
            if (entered)
                counter.fetch_sub(1, std::memory_order_release);
        }
    } scope(impl_->concurrent_pumpers);

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

    // Slice A (task #215) — admission lookup + decision under shared
    // lock.  Previously the shared_lock covered only the lookup;
    // `admission->is_peer_allowed(...)` ran AFTER the lock released,
    // with a raw pointer that could dangle.  Now the shared_lock
    // spans both lookup and admission.
    //
    // Architectural reason: `admission` is a raw pointer to the
    // PeerAdmission registered by `register_domain`.  In production
    // this points at a `ZmqQueue`; the queue's destruction goes
    // through `~ZapDomainHandle` → `unregister_domain_`, which takes
    // the same mutex in unique mode.  Holding shared across the
    // admission call ensures the destructor's unique_lock blocks
    // until this thread releases — the queue cannot be freed
    // mid-decision.  Without this scope extension, every handshake-
    // vs-teardown race is a UAF the moment AUTH-2 (#162) wires
    // pump_one to the BRC poll thread.
    //
    // RecursionGuard keyed on `this` enforces the second half of the
    // is_peer_allowed contract: implementers MUST NOT call back into
    // `register_domain` (refused via `is_recursing`) or trigger
    // destruction of any ZapDomainHandle (`~ZapDomainHandle` would
    // route through `unregister_domain_` which PANICs).  Per-thread
    // detection (recursion_guard.hpp is `thread_local`) — concurrent
    // calls from OTHER threads are unaffected.
    //
    // try/catch around the admission call: future implementers may
    // throw; treat the throw as a deny + log, since the alternative
    // is an unhandled exception bubbling to the BRC poll thread.
    bool ok               = false;
    bool admission_found  = false;
    {
        std::shared_lock<std::shared_mutex> lk(impl_->registered_mu);
        if (auto it = impl_->registered.find(domain);
            it != impl_->registered.end())
        {
            admission_found = true;
            PeerAdmission *admission = it->second;
            pylabhub::basics::RecursionGuard guard(this);
            try
            {
                ok = admission->is_peer_allowed(
                    PeerIdentity{"curve", z85});
            }
            catch (const std::exception &e)
            {
                LOGGER_ERROR("ZapRouter::pump_one: admission threw — "
                             "treating as deny.  what(): {}", e.what());
                ok = false;
            }
            catch (...)
            {
                LOGGER_ERROR("ZapRouter::pump_one: admission threw "
                             "non-std exception — treating as deny.");
                ok = false;
            }
        }
    }  // shared_lock + RecursionGuard release here

    if (!admission_found)
    {
        LOGGER_WARN("ZapRouter::pump_one: rejecting handshake — no "
                    "domain registered for '{}' (either misconfigured "
                    "zap_domain or unsolicited probe)", domain);
        send_zap_reply(sock, request_id, "400",
                       "Domain not registered", "");
        impl_->denied_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

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
