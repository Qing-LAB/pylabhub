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
#include "utils/recursion_guard.hpp"  // RecursionGuard — task #215
#include "utils/security/domain_routing_table.hpp"  // task #219
#include "utils/zmq_context.hpp"
#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"   // recv_multipart

#include <zmq.h>  // zmq_z85_encode

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>  // std::stop_token paired with std::jthread
#include <string>
#include <string_view>
#include <thread>
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
void send_zap_reply(zmq::socket_t    &sock,
                    std::string_view  request_id,
                    std::string_view  status_code,
                    std::string_view  status_text,
                    std::string_view  user_id)
{
    const std::array<zmq::const_buffer, 6> frames{
        zmq::buffer(kZapVersion, std::char_traits<char>::length(kZapVersion)),
        zmq::buffer(request_id.data(),  request_id.size()),
        zmq::buffer(status_code.data(), status_code.size()),
        zmq::buffer(status_text.data(), status_text.size()),
        zmq::buffer(user_id.data(),     user_id.size()),
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
    /// Admission dispatch table.  Encapsulates the routing map +
    /// shared_mutex + lock-bounded callback invariants previously
    /// inlined here.  See `utils/security/domain_routing_table.hpp`.
    /// The `ZapDomainHandle` returned by `register_domain` keeps the
    /// mapping alive only for as long as the queue (or other
    /// admission-bearing surface) needs it; the handle's destructor
    /// removes the entry BEFORE the admission referent destructs.
    DomainRoutingTable routing;

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

    /// task #215 — single-pumper invariant counter.  Incremented on
    /// entry to pump_one; PANIC if post-increment > 1.  See
    /// zap_router.hpp threading model + HEP-CORE-0036 §7.4.
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

void ZapRouter::ensure_module_registered()
{
    // Same pattern as register_domain's registration block, but
    // without the domain bookkeeping.  Used by ZapPumpThread to
    // guarantee its dep is in the registry before LoadModule runs.
    auto &self = instance();
    std::lock_guard<std::mutex> lk(self.impl_->lifecycle_mu);
    if (self.impl_->module_registered.load(std::memory_order_acquire))
        return;
    if (!pylabhub::utils::LifecycleManager::instance()
            .register_dynamic_module(make_module_def_()))
    {
        LOGGER_WARN("ZapRouter::ensure_module_registered: "
                    "register_dynamic_module returned false — module "
                    "may already be registered in this lifecycle cycle");
    }
    self.impl_->module_registered.store(true, std::memory_order_release);
}

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
    // NOTE: ZapRouter does NOT depend on SecureSubsystem.  ZAP handles
    // ZMQ CURVE negotiation via libzmq's internal path; libzmq lazy-
    // initializes libsodium on its own.  Adding a SecureSubsystem dep
    // was tried on 2026-07-07 and reverted — it caused ZapRouterTest
    // failures because those tests do not stand up SMS.
    def.set_startup(ZapRouter::lifecycle_startup_thunk);
    def.set_shutdown(ZapRouter::lifecycle_shutdown_thunk,
                     std::chrono::milliseconds(500));
    def.set_as_persistent(true);
    return def;
}

// ── register_domain / unregister_domain_ ────────────────────────────────────

ZapDomainHandle
ZapRouter::register_domain(std::string domain, PeerAdmission &admission)
{
    if (domain.empty())
        throw std::runtime_error(
            "ZapRouter::register_domain: domain must be non-empty");
    // Non-null contract is enforced by the reference parameter — a
    // caller cannot pass a null admission.

    // task #215 — reentrance refuse.  MUST run BEFORE any lock
    // acquisition: if this thread is inside pump_one's admission
    // scope, the routing table's shared_lock is held; re-acquiring
    // shared OR unique from the same thread is std::shared_mutex UB.
    // See peer_admission.hpp `is_peer_allowed` reentrance contract +
    // HEP-CORE-0036 §7.4 invariant 2.
    if (pylabhub::basics::RecursionGuard::is_recursing(this))
    {
        LOGGER_ERROR("ZapRouter::register_domain: reentrant call "
                     "detected from inside a PeerAdmission decision "
                     "(domain='{}').  Returning inactive handle.",
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

    if (!impl_->routing.register_domain(domain, admission))
    {
        // Roll back the LoadModule to keep ref-counts accurate.
        (void) pylabhub::utils::UnloadModule(kZapModuleName);
        throw std::runtime_error(
            "ZapRouter::register_domain: domain '" + domain +
            "' is already registered (double-registration is a "
            "regression — each zap_domain must be unique)");
    }

    return ZapDomainHandle(this, std::move(domain));
}

void ZapRouter::unregister_domain_(const std::string &domain)
{
    if (domain.empty()) return;

    // task #215 — reentrance PANIC.  A destructor can't react to a
    // refusal; the only honest response is to abort.  See
    // peer_admission.hpp `is_peer_allowed` reentrance contract +
    // HEP-CORE-0036 §7.4 invariant 2.
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

    impl_->routing.unregister_domain(domain);
    (void) pylabhub::utils::UnloadModule(kZapModuleName);
}

std::size_t ZapRouter::registered_domain_count_for_test() const
{
    return impl_->routing.size();
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
    // task #215 — single-pumper invariant.  libzmq's REP socket FSM
    // is single-thread; concurrent pumpers silently corrupt it.  RAII
    // counter PANICs on second concurrent entry.  See zap_router.hpp
    // threading model + HEP-CORE-0036 §7.4 invariant 3.  The PANIC-
    // branch decrement keeps the counter coherent for any post-PANIC
    // observer; dtor's `entered` flag suppresses double-decrement on
    // the (currently unreachable) path where PLH_PANIC ever became a
    // throw rather than abort.
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
                counter.fetch_sub(1, std::memory_order_release);
                PLH_PANIC("ZapRouter::pump_one: concurrent pumper "
                          "detected (post-increment count = {}).  "
                          "Production wires exactly ONE pumper (BRC "
                          "poll thread per HEP-CORE-0036 §7.1); tests "
                          "use ZapPumpThread (also one).", post);
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

    // task #215 — DomainRoutingTable::with_admission holds the
    // shared_lock across the admission call so ~ZapDomainHandle
    // (which routes through unregister_domain_ → unique_lock) cannot
    // free the admission referent mid-decision.  RecursionGuard keyed
    // on `this` enforces the is_peer_allowed contract: implementers
    // MUST NOT call back into register_domain (refused) or trigger
    // ~ZapDomainHandle (PANIC).  The table also catches throws from
    // the admission body and translates to false (deny) so a future
    // throwing implementer cannot crash the BRC poll thread.
    // See HEP-CORE-0036 §7.4 + peer_admission.hpp.
    const std::optional<bool> decision = impl_->routing.with_admission(
        domain, this, [&](PeerAdmission &admission) {
            return admission.is_peer_allowed(
                PeerIdentity{"curve", z85});
        });

    if (!decision.has_value())
    {
        LOGGER_WARN("ZapRouter::pump_one: rejecting handshake — no "
                    "domain registered for '{}' (either misconfigured "
                    "zap_domain or unsolicited probe)", domain);
        send_zap_reply(sock, request_id, "400",
                       "Domain not registered", "");
        impl_->denied_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (*decision)
    {
        // Symmetric to the existing deny-path WARN logs (lines 433,
        // 450, 463, 476, 503): one-shot INFO per allowed handshake
        // makes ZAP behavior observable for ops debugging without
        // flooding hot paths (handshakes are bounded by role count).
        LOGGER_INFO("ZapRouter::pump_one: ALLOW pubkey='{}' on domain='{}'",
                    z85, domain);
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

// `std::jthread` carries its own stop_token + auto-joins in its
// destructor.  The previous shape required a separate atomic stop flag
// plus a manual join in `~ZapPumpThread`; both are now implicit.
struct ZapPumpThread::Impl
{
    std::jthread thread;
};

ZapPumpThread::ZapPumpThread(std::chrono::milliseconds tick)
    : impl_(std::make_unique<Impl>())
{
    impl_->thread = std::jthread([tick](std::stop_token st) {
        while (!st.stop_requested())
        {
            // pump_one returns false on timeout OR module-not-loaded.
            // Either way we just spin to the next tick to check stop.
            (void) ZapRouter::instance().pump_one(tick);
        }
    });
}

ZapPumpThread::~ZapPumpThread() = default;

// ── ZapPumpThread lifecycle module API ─────────────────────────────────────
//
// Process-singleton pump owned by the LifecycleManager.  Two activation
// modes coexist on the SAME class:
//
//   * Direct RAII (tests, demos): `ZapPumpThread pump;` — destructor joins.
//   * Lifecycle module (production: `plh_role_main`): startup thunk
//     emplaces `g_module_pump_`, shutdown thunk destroys it.
//
// `pump_one`'s atomic counter PLH_PANICs if both modes are active in the
// same process — that's the single-pumper safety net per HEP-CORE-0036 §7.4.
// We intentionally do NOT add a soft preflight check here: the runtime
// PANIC is the canary the design relies on.  See zap_router.hpp's
// "Two activation modes" docstring on `ZapPumpThread`.

constexpr const char *kZapPumpThreadModuleName = "ZapPumpThread";

namespace {

// File-scope singleton storage for the lifecycle-module-managed pump.
// std::optional gives us emplace/reset semantics matching the
// startup/shutdown thunks.  Mutex guards against the (unlikely)
// concurrent LoadModule / UnloadModule race.
std::mutex                       g_module_mu_;
std::optional<ZapPumpThread>     g_module_pump_;
std::atomic<bool>                g_module_registered_{false};

} // namespace

const char *ZapPumpThread::module_name() noexcept
{
    return kZapPumpThreadModuleName;
}

void ZapPumpThread::lifecycle_startup_thunk(const char * /*name*/,
                                              void * /*userdata*/)
{
    std::lock_guard<std::mutex> lk(g_module_mu_);
    if (g_module_pump_.has_value())
        return;  // idempotent — persistent module may re-startup
    g_module_pump_.emplace();   // default tick (100 ms shutdown cadence)
    LOGGER_INFO("[ZapPumpThread] event=ModuleStartup pumper running "
                "(production path; pumps {})", kZapModuleName);
}

void ZapPumpThread::lifecycle_shutdown_thunk(const char * /*name*/,
                                               void * /*userdata*/)
{
    std::lock_guard<std::mutex> lk(g_module_mu_);
    if (!g_module_pump_.has_value())
        return;
    g_module_pump_.reset();     // jthread destructor joins
    LOGGER_INFO("[ZapPumpThread] event=ModuleShutdown pumper joined");
    // Allow re-registration in a future LifecycleGuard cycle.
    g_module_registered_.store(false, std::memory_order_release);
}

pylabhub::utils::ModuleDef ZapPumpThread::make_module_def_()
{
    pylabhub::utils::ModuleDef def(kZapPumpThreadModuleName);
    // Depend on ZapRouter so the dep graph guarantees:
    //   startup order: ZapRouter starts → ZapPumpThread starts
    //   shutdown order: ZapPumpThread stops → ZapRouter stops
    // No risk of pumping a destroyed REP socket.
    def.add_dependency(kZapModuleName);
    def.set_startup(ZapPumpThread::lifecycle_startup_thunk);
    def.set_shutdown(ZapPumpThread::lifecycle_shutdown_thunk,
                     std::chrono::milliseconds(500));
    // Persistent: don't tear down on ref_count==0; only on finalize.
    def.set_as_persistent(true);
    return def;
}

void ZapPumpThread::ensure_registered_and_loaded()
{
    // Ensure our dep (ZapRouter) is in the dynamic registry first.
    // ZapRouter registers itself lazily on `register_domain`, but
    // we're loading at role-startup BEFORE any CURVE socket has been
    // created; without this preflight, LoadModule below would fail
    // to resolve the dep.  Idempotent — second-and-later calls are
    // no-ops.
    ZapRouter::ensure_module_registered();

    // Register the dynamic module exactly once per process.  Subsequent
    // calls are no-ops (idempotent).  Lazy bind to LifecycleManager —
    // we don't drag the dep in at static-init time.
    {
        std::lock_guard<std::mutex> lk(g_module_mu_);
        if (!g_module_registered_.load(std::memory_order_acquire))
        {
            if (!pylabhub::utils::LifecycleManager::instance()
                    .register_dynamic_module(make_module_def_()))
            {
                LOGGER_WARN(
                    "ZapPumpThread: register_dynamic_module returned "
                    "false — module may already be registered in this "
                    "lifecycle cycle");
            }
            g_module_registered_.store(true,
                                         std::memory_order_release);
        }
    }

    // LoadModule pulls in ZapRouter as a dependency (which binds the
    // inproc REP socket) before running our startup thunk (which
    // emplaces the pump thread).  Throws on failure — a role that
    // can't authenticate CURVE clients shouldn't keep running.
    if (!pylabhub::utils::LoadModule(kZapPumpThreadModuleName))
        throw std::runtime_error(
            "ZapPumpThread::ensure_registered_and_loaded: LoadModule('" +
            std::string(kZapPumpThreadModuleName) + "') failed");
}

} // namespace pylabhub::utils::security
