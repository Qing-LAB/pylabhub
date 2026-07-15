/**
 * @file broker_request_comm.cpp
 * @brief BrokerRequestComm — role-to-broker ZMQ DEALER protocol.
 *
 * Implements the broker protocol using a single DEALER socket, MonitoredQueue
 * command queue, and the redesigned ZmqPollLoop with inproc wake-up.
 */

#include "utils/broker_request_comm.hpp"
#include "utils/security/curve_keypair.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"

#include "plh_platform.hpp"   // platform::get_pid()
#include "utils/logger.hpp"
#include "utils/wire_adapter.hpp"
#include "utils/wire_bodies.hpp"   // WireBodyError
#include "utils/wire_envelope.hpp"
#include "utils/zmq_context.hpp"
#include "utils/zmq_poll_loop.hpp"
#include "utils/zmq_socket_policy.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"

#include "monitored_queue.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>

namespace pylabhub::hub
{

// ============================================================================
// Wire protocol helpers (HEP-CORE-0046 §14)
// ============================================================================

namespace
{

/// Generate a fresh 16-byte random hex correlation_id (32 hex chars).
/// Cryptographically random per I-CORRELATION-STABLE + I-REPLAY-BOUND —
/// nonce reuse must be statistically impossible so the broker's nonce-
/// dedup window can't collide across roles.
std::string make_random_hex16()
{
    std::array<std::uint8_t, 16> raw{};
    namespace sec = pylabhub::utils::security;
    sec::secure().random_bytes(raw);
    // 2 hex chars per byte + NUL terminator.
    char hex[33] = {};
    sec::secure().bin2hex(hex, sizeof(hex), raw.data(), raw.size());
    return std::string(hex, 32);
}

std::uint64_t system_wall_now_ms()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch())
            .count());
}

} // anonymous namespace

// ============================================================================
// Command types for the MonitoredQueue
// ============================================================================

namespace
{

/// Fire-and-forget: send the 5-frame envelope (HEP-CORE-0046 §14).
/// Per I-CORRELATION-STABLE, correlation_id is non-empty for non-NOTIFY
/// msg_types; the sender site generates one via `make_random_hex16()`.
/// For REG-family msg_types (see §14.3 / kRegFamilyMsgTypes in
/// wire_adapter.cpp) the sender also fills in the security triple —
/// `client_nonce` + `client_wall_ts` — per I-REPLAY-BOUND.
struct SendCmd
{
    std::string    msg_type;
    std::string    correlation_id;
    std::string    client_nonce;
    std::uint64_t  client_wall_ts{0};
    nlohmann::json payload;
};

/// Request-reply: send 5 frames, wait for matching reply.
struct RequestCmd
{
    std::string              msg_type;
    /// Expected reply msg_types (e.g. ["REG_ACK"] or ["DISC_ACK", "DISC_PENDING"]).
    /// Any reply matching one of these is accepted as a response for this request.
    std::vector<std::string> expected_acks;
    /// Non-empty per I-CORRELATION-STABLE — the wire authority for reply
    /// matching.  Broker echoes it in Frame 3 of the ACK; BRC's
    /// pending_requests map keys on this value.
    std::string              correlation_id;
    /// Set for REG-family REQs per I-REPLAY-BOUND.  Empty on other types.
    std::string              client_nonce;
    std::uint64_t            client_wall_ts{0};
    nlohmann::json           payload;

    // Signaling: broker thread sets result and notifies.
    std::mutex                        mu;
    std::condition_variable           cv;
    bool                              done{false};
    std::optional<nlohmann::json>     result;
    /// Set by the CALLER thread on client-side timeout.  Read by
    /// the CTRL thread's poll_recv: if a late broker reply arrives
    /// AFTER `abandoned=true`, the reply is dropped (as if it never
    /// arrived) and the entry is removed from `pending_requests`.
    /// This prevents cross-wiring of a stale reply into the caller's
    /// NEXT sync REQ of the same ack type — HEP-CORE-0042 §7.1
    /// fan-in loop review fix (2026-07-02).  Atomic because the
    /// timeout write and the poll_recv read are on different threads
    /// with no other synchronization.
    std::atomic<bool>                 abandoned{false};
};

/// Install a periodic task into the running poll loop.
/// All installs (pre-startup + post-startup) flow through this command so
/// `pImpl->periodic_tasks` storage is unnecessary and the post-REG_ACK
/// heartbeat-cadence negotiation (HEP-CORE-0023 §2.5) works without
/// special-casing.
struct InstallPeriodicTaskCmd
{
    std::function<void()>     action;
    int                       interval_ms{0};
    std::function<uint64_t()> get_iteration; // optional (nullptr = time-only)
};

using BrokerCommand = std::variant<SendCmd,
                                    std::shared_ptr<RequestCmd>,
                                    InstallPeriodicTaskCmd>;

} // anonymous namespace

// ============================================================================
// Impl
// ============================================================================

struct BrokerRequestComm::Impl
{
    // DEALER socket + monitor.
    std::optional<zmq::socket_t> dealer;
    std::optional<zmq::socket_t> monitor_sock;
    std::string                  monitor_endpoint;
    // Broker endpoint cached from Config for log lines (esp. the
    // terminal-disconnect WARN — operators reading multi-hub roles
    // need to know WHICH broker just died).  Set once at init.
    std::string                  broker_endpoint;

    // Inproc PAIR for wake-up.
    std::optional<zmq::socket_t> signal_read;
    std::optional<zmq::socket_t> signal_write;

    // Command queue.
    MonitoredQueue<BrokerCommand> cmd_queue;

    // Pending request-reply map: keyed by correlation_id (Frame 3 of
    // the incoming reply per HEP-CORE-0046 §14 I-CORRELATION-STABLE).
    // Each REQ generates a fresh correlation_id in `do_request_multi`;
    // the broker echoes it back on the ACK / ERROR reply.  Only ONE
    // entry per RequestCmd (not per expected_ack) since correlation_id
    // is unique per REQ regardless of how many ack types the caller
    // will accept.  ERROR replies also carry the same correlation_id
    // (broker's `make_error` populates it from the request body),
    // so no cross-wiring against pending REQs.
    std::unordered_map<std::string, std::shared_ptr<RequestCmd>> pending_requests;

    /// HEP-CORE-0007 §12.2.1 shape-conformance observability counter.
    /// Incremented when the receive loop observes a reply-shape message
    /// (msg_type ends in `_ACK` or is exactly `ERROR`) for which no
    /// pending request is registered.  This is a *runtime* fingerprint
    /// of a shape-contract violation: either the broker emitted an ACK
    /// for a fire-and-forget REQ (server side bug), or the BRC client
    /// sent a request via cmd_queue.push that should have been
    /// do_request (client side bug — the ENDPOINT_UPDATE half-mix
    /// shipped 2026-05-21).  The reply gets silently dropped (logged
    /// as WARN); this counter makes the drop observable so tests and
    /// monitoring can catch the regression.
    std::atomic<size_t> unmatched_replies_count{0};

    // Callbacks.
    NotificationCallback on_notification_cb;
    std::function<void()> on_hub_dead_cb;

    // Identity (from Config, used by channel join/leave/msg).
    std::string role_uid;
    std::string role_name;

    // Periodic tasks: owned by the active poll-loop's local vector.
    // active_loop_periodic_tasks is set while run_poll_loop is running so
    // InstallPeriodicTaskCmd can append at any time (pre- or post-startup).
    // The pointer is only ever read/written from the ctrl thread (the same
    // thread that runs the poll loop and drains the cmd queue), so no
    // atomic is needed for the pointer itself.
    std::vector<scripting::PeriodicTask> *active_loop_periodic_tasks{nullptr};

    // State.
    std::atomic<bool> connected{false};
    std::atomic<bool> stop_requested{false};

    // ── Wire protocol helpers ──────────────────────────────────────────

    /// Build + send the 5-frame wire envelope (HEP-CORE-0046 §14).
    /// The envelope encoder stamps `envelope_hash` and — for REG-family
    /// msg_types — the security triple `client_nonce` + `client_wall_ts`
    /// into the body per I-REPLAY-BOUND before serializing.  Callers
    /// generate `correlation_id` (and the triple for REG-family) at the
    /// REQ construction site; SendCmd / RequestCmd carry them here.
    void send_message(const std::string    &msg_type,
                      const std::string    &correlation_id,
                      const std::string    &client_nonce,
                      std::uint64_t         client_wall_ts,
                      const nlohmann::json &payload)
    {
        if (!dealer)
            return;
        // Audit S1 (2026-05-18) — layer 1 of the 4-layer time-bound
        // model (HEP-0023 §2.5.3).  After ZMQ_EVENT_DISCONNECTED has
        // flipped `connected` to false, skip the send immediately —
        // with reconnect_ivl=-1 there's no future peer to deliver to,
        // and libzmq's send would block waiting for a peer until
        // sndtimeo (500ms) fires.  Skipping early saves the wait and
        // makes the role-side caller's per-request timeout the
        // canonical "did this work?" signal (layer 3).
        if (!connected.load(std::memory_order_acquire))
        {
            LOGGER_WARN("BrokerRequestComm[{}]: send '{}' suppressed — "
                        "connection terminally dead (policy)",
                        broker_endpoint, msg_type);
            return;
        }
        try
        {
            ::pylabhub::wire::adapter::EncodeContext ctx;
            ctx.dealer_role_uid = role_uid;
            ctx.correlation_id  = correlation_id;
            ctx.client_nonce    = client_nonce;
            ctx.client_wall_ts  = client_wall_ts;
            zmq::multipart_t wire =
                ::pylabhub::wire::adapter::encode_dealer_send(
                    msg_type, ctx, payload);
            wire.send(*dealer);
        }
        catch (const zmq::error_t &e)
        {
            LOGGER_WARN("BrokerRequestComm: send '{}' failed: {}",
                        msg_type, e.what());
        }
        catch (const ::pylabhub::wire::WireBodyError &e)
        {
            // Encoder-side contract violation.  This is a programmer bug
            // (empty correlation_id on a non-NOTIFY, missing security
            // triple on a REG-family REQ) — log LOUD and let the caller
            // observe the missing reply as a timeout.
            LOGGER_ERROR("BrokerRequestComm: envelope encoder rejected '{}': {}",
                         msg_type, e.what());
        }
    }

    void recv_and_dispatch()
    {
        if (!dealer)
            return;

        // Non-blocking receive all pending messages.
        while (true)
        {
            zmq::multipart_t msg;
            if (!msg.recv(*dealer, ZMQ_DONTWAIT))
                break;
            if (msg.empty())
                break;

            // 4-frame envelope on DEALER receive (libzmq strips Frame 0
            // during routing per WireEnvelope::parse_dealer_recv contract).
            ::pylabhub::wire::ParseError err = {};
            auto env_opt = ::pylabhub::wire::WireEnvelope::parse_dealer_recv(
                std::move(msg), role_uid, &err);
            if (!env_opt.has_value())
            {
                LOGGER_WARN("BrokerRequestComm[{}]: envelope parse rejected "
                            "reply (ParseError={})",
                            role_name, static_cast<int>(err));
                continue;
            }
            ::pylabhub::wire::WireEnvelope env = std::move(*env_opt);
            const std::string msg_type       = std::string(env.msg_type());
            const std::string correlation_id = std::string(env.correlation_id());
            // Materialize a mutable body copy + inject the envelope
            // correlation_id so legacy handlers reading
            // `body.value("correlation_id","")` keep working unchanged
            // (adapter Tier 2 contract).
            nlohmann::json body = env.body();
            if (!correlation_id.empty())
                body["correlation_id"] = correlation_id;

            // Match this reply to a pending request by correlation_id
            // per I-CORRELATION-STABLE.  ERROR replies carry the same
            // correlation_id (broker's make_error populates it from the
            // request body), so no per-msg_type ERROR fallback needed.
            if (!correlation_id.empty())
            {
                auto it = pending_requests.find(correlation_id);
                if (it != pending_requests.end())
                {
                    auto req = it->second;
                    pending_requests.erase(it);
                    // HEP-CORE-0042 Phase 3 review fix (2026-07-02):
                    // if the caller has ABANDONED this request (client-
                    // side timeout fired), drop the reply on the floor
                    // rather than delivering it to the caller's next
                    // sync REQ of the same ack type.  The caller has
                    // already returned nullopt and moved on; delivering
                    // this stale reply would cross-wire it.
                    if (req->abandoned.load(std::memory_order_acquire))
                    {
                        LOGGER_WARN("BrokerRequestComm: late reply for '{}' "
                                    "(corr_id='{}') arrived after caller-"
                                    "side timeout; dropping to prevent "
                                    "cross-wire",
                                    msg_type, correlation_id);
                        continue;
                    }
                    {
                        std::lock_guard<std::mutex> lk(req->mu);
                        req->result = std::move(body);
                        req->done = true;
                    }
                    req->cv.notify_one();
                    continue;
                }
            }

            // Shape-conformance check (HEP-CORE-0007 §12.2.1): a
            // reply-shape message (`*_ACK` or `ERROR`) at this point
            // has no pending waiter — silently delivering it to
            // `on_notification_cb` would mask a real protocol bug
            // (unexpected ACK or ERROR with an unknown correlation_id).
            // Count + WARN so the drop is observable.
            const bool is_reply_shape =
                (msg_type.size() >= 4 &&
                 msg_type.compare(msg_type.size() - 4, 4, "_ACK") == 0) ||
                msg_type == "ERROR";
            if (is_reply_shape)
            {
                unmatched_replies_count.fetch_add(
                    1, std::memory_order_relaxed);
                LOGGER_WARN("BrokerRequestComm: received unexpected "
                            "reply-shape message with no pending "
                            "request: type='{}' corr_id='{}' (HEP-0007 "
                            "§12.2.1 shape contract violation)",
                            msg_type, correlation_id);
                continue;
            }

            // Unsolicited notification — dispatch to callback.
            LOGGER_INFO("[brc/{}] event=BrcNotifyReceived type='{}' "
                        "role_uid='{}' has_callback={}",
                        role_name, msg_type, role_uid,
                        on_notification_cb ? "true" : "false");
            if (on_notification_cb)
                on_notification_cb(msg_type, body);
        }
    }

    void check_monitor()
    {
        if (!monitor_sock)
            return;

        // Non-blocking poll the monitor socket.
        zmq::pollitem_t pi{};
        pi.socket = monitor_sock->handle();
        pi.events = ZMQ_POLLIN;
        try
        {
            zmq::poll(&pi, 1, std::chrono::milliseconds{0});
        }
        catch (const zmq::error_t &)
        {
            return;
        }
        if (!(pi.revents & ZMQ_POLLIN))
            return;

        // Monitor frame: [event_data(6 bytes)] [address]
        zmq::message_t ev_msg, addr_msg;
        try
        {
            (void)monitor_sock->recv(ev_msg, zmq::recv_flags::dontwait);
            (void)monitor_sock->recv(addr_msg, zmq::recv_flags::dontwait);
        }
        catch (const zmq::error_t &)
        {
            return;
        }

        if (ev_msg.size() >= 6) // ZMQ monitor frame: uint16 event_id + uint32 value
        {
            const auto *data = static_cast<const uint8_t *>(ev_msg.data());
            const uint16_t event_id =
                static_cast<uint16_t>(data[0]) |
                (static_cast<uint16_t>(data[1]) << 8);
            if (event_id == ZMQ_EVENT_DISCONNECTED)
            {
                // Audit S1 (2026-05-18) — pylabhub policy:
                // disconnect is TERMINAL.  reconnect_ivl=-1 (set
                // by apply_socket_policy at socket init) prevents
                // auto-recovery; the role-side `on_hub_dead_cb`
                // runs next and either stops the role (default)
                // or fires the script's `on_hub_dead` override.
                // See HEP-CORE-0023 §2.5.3 "Disconnection is
                // terminal".
                //
                // Flip `connected = false` BEFORE firing the
                // callback so any concurrent `send_message` calls
                // observe the dead state and skip enqueueing
                // doomed messages (layer 1 of the 4-layer model).
                connected.store(false, std::memory_order_release);
                LOGGER_WARN("BrokerRequestComm[{}]: ZMQ_EVENT_DISCONNECTED — "
                            "connection TERMINAL (auto-reconnect disabled by "
                            "policy); role-side on_hub_dead will fire next",
                            broker_endpoint);
                if (on_hub_dead_cb)
                    on_hub_dead_cb();
            }
        }
    }

    void drain_command_queue()
    {
        cmd_queue.drain([this](BrokerCommand &cmd) {
            std::visit([this](auto &c) { handle_command(c); }, cmd);
        });
    }

    void handle_command(SendCmd &cmd)
    {
        send_message(cmd.msg_type, cmd.correlation_id,
                     cmd.client_nonce, cmd.client_wall_ts, cmd.payload);
    }

    void handle_command(std::shared_ptr<RequestCmd> &cmd)
    {
        // Key on correlation_id per I-CORRELATION-STABLE — the wire
        // authority for reply matching.  expected_acks stays as a
        // caller-side sanity list (e.g. DISC accepts DISC_ACK or
        // DISC_PENDING) but the wire uses correlation_id alone.
        pending_requests[cmd->correlation_id] = cmd;
        send_message(cmd->msg_type, cmd->correlation_id,
                     cmd->client_nonce, cmd->client_wall_ts, cmd->payload);
    }

    void handle_command(InstallPeriodicTaskCmd &cmd)
    {
        // Drained on the ctrl thread (same thread as the poll loop's tick),
        // so direct emplace into the active loop's vector is safe.
        if (!active_loop_periodic_tasks)
        {
            LOGGER_WARN("BrokerRequestComm: InstallPeriodicTaskCmd dropped — "
                        "no active poll loop");
            return;
        }
        active_loop_periodic_tasks->emplace_back(
            std::move(cmd.action), cmd.interval_ms, std::move(cmd.get_iteration));
    }

    /// Submit a request-reply and block until reply or timeout.
    /// Single expected ack — convenience wrapper.
    std::optional<nlohmann::json> do_request(
        const std::string &msg_type,
        const std::string &expected_ack,
        nlohmann::json payload,
        int timeout_ms)
    {
        return do_request_multi(msg_type, {expected_ack},
                                std::move(payload), timeout_ms);
    }

    /// Submit a request-reply accepting one of several reply types.
    /// Used by DISC_REQ which may reply with DISC_ACK or DISC_PENDING.
    std::optional<nlohmann::json> do_request_multi(
        const std::string &msg_type,
        std::vector<std::string> expected_acks,
        nlohmann::json payload,
        int timeout_ms)
    {
        auto req = std::make_shared<RequestCmd>();
        req->msg_type       = msg_type;
        req->expected_acks  = std::move(expected_acks);
        // Caller-provided `payload["correlation_id"]` (if a non-empty
        // string) wins; otherwise generate a fresh 32-hex value.
        // Non-empty per I-CORRELATION-STABLE.  Tests that pin a
        // specific correlation_id (WireConformance BandCorrIdEcho,
        // Band CorrIdEcho, etc.) rely on this pass-through — the
        // caller-set value must reach Frame 3 unchanged so broker's
        // ACK echoes it verbatim.
        {
            std::string caller_corr;
            if (payload.is_object())
            {
                auto it = payload.find("correlation_id");
                if (it != payload.end() && it->is_string())
                    caller_corr = it->get<std::string>();
            }
            req->correlation_id = caller_corr.empty()
                ? make_random_hex16()
                : std::move(caller_corr);
        }
        // Security triple for REG-family REQs per I-REPLAY-BOUND.  The
        // encoder validates non-empty nonce + non-zero wall_ts and
        // stamps them into the body before serializing.
        if (::pylabhub::wire::adapter::msg_type_carries_security_triple(msg_type))
        {
            req->client_nonce   = make_random_hex16();
            req->client_wall_ts = system_wall_now_ms();
        }
        req->payload = std::move(payload);

        cmd_queue.push(req);

        std::unique_lock<std::mutex> lk(req->mu);
        if (!req->cv.wait_for(lk, std::chrono::milliseconds{timeout_ms},
                              [&] { return req->done; }))
        {
            LOGGER_WARN("BrokerRequestComm: {} timed out after {}ms "
                        "(corr_id='{}')",
                        msg_type, timeout_ms, req->correlation_id);
            // HEP-CORE-0042 Phase 3 review fix (2026-07-02) — mark
            // the request ABANDONED so if a late broker reply arrives
            // (client-side timeout fired but the reply was in flight),
            // poll_recv drops it on the floor instead of cross-wiring
            // it into the caller's NEXT sync REQ of the same ack type.
            // Without this, iteration N's stale reply could satisfy
            // iteration N+1 of the fan-in loop in apply_consumer_reg_ack.
            req->abandoned.store(true, std::memory_order_release);
            return std::nullopt;
        }
        return req->result;
    }
};

// ============================================================================
// Lifecycle
// ============================================================================

BrokerRequestComm::BrokerRequestComm()
    : pImpl(std::make_unique<Impl>())
{}

BrokerRequestComm::~BrokerRequestComm()
{
    disconnect();
}

BrokerRequestComm::BrokerRequestComm(BrokerRequestComm &&) noexcept = default;
BrokerRequestComm &BrokerRequestComm::operator=(BrokerRequestComm &&) noexcept = default;

bool BrokerRequestComm::connect(const Config &cfg)
{
    if (pImpl->connected.load(std::memory_order_acquire))
        return true;

    if (cfg.broker_endpoint.empty())
    {
        LOGGER_ERROR("BrokerRequestComm: broker_endpoint cannot be empty");
        return false;
    }

    try
    {
        auto &ctx = get_zmq_context();

        // Create DEALER socket + apply pylabhub's standard ZMQ socket
        // policy (linger=0, sndtimeo=500ms, heartbeat 5s/30s,
        // reconnect_ivl=-1).  Policy contract: HEP-CORE-0023 §2.5.3
        // "Disconnection is terminal".  Implementation: helper in
        // `utils/zmq_socket_policy.hpp`.  Subsystem-specific options
        // (CURVE, ROUTING_ID, HWMs, etc.) are set below.
        pImpl->dealer.emplace(ctx, zmq::socket_type::dealer);
        ::pylabhub::utils::apply_socket_policy(
            *pImpl->dealer,
            ::pylabhub::utils::ZmqSocketRole::TcpConnect);

        // CurveZMQ — unconditional per HEP-CORE-0035 §2.  All three
        // fields are required; the broker's CTRL ROUTER will reject
        // an unauthenticated DEALER (CURVE-server + ZAP gate at the
        // remote).  Empty here is a programmer error (no-bypass
        // discipline, §4.6.5): the role loaded its vault but failed
        // to populate the BRC config — surface the misconfiguration
        // at connect() rather than producing a stuck handshake.
        // HEP-CORE-0040 §172: read the role's CURVE identity on-site
        // from secure().keys() by the caller-supplied keystore_name.
        // Absence → refuse the connect (loud, not silent).
        if (cfg.broker_pubkey.empty()
         || cfg.keystore_name.empty()
         || !pylabhub::utils::security::sodium_ready()
         || !pylabhub::utils::security::secure().keys().has(cfg.keystore_name))
        {
            LOGGER_ERROR(
                "BrokerRequestComm: broker_pubkey empty or KeyStore entry "
                "'{}' absent (HEP-CORE-0035 §2; HEP-CORE-0040 §172).  "
                "Refusing to connect to {} without CURVE.",
                cfg.keystore_name, cfg.broker_endpoint);
            pImpl->dealer.reset();
            return false;
        }
        auto &ks = pylabhub::utils::security::secure().keys();
        // One-shot per BRC::connect — surface the CURVE values the
        // socket was configured with so silent handshake failures
        // (wrong serverkey, missing client identity) have an
        // observable diagnostic trail.  Volume is bounded by hub
        // count per role (typically 1).
        LOGGER_INFO("BrokerRequestComm::connect: CURVE configured — "
                    "serverkey='{}' client_pubkey='{}' "
                    "(KeyStore['{}']) endpoint='{}'",
                    cfg.broker_pubkey,
                    ks.pubkey(cfg.keystore_name),
                    cfg.keystore_name,
                    cfg.broker_endpoint);
        pImpl->dealer->set(zmq::sockopt::curve_serverkey, cfg.broker_pubkey);
        pImpl->dealer->set(zmq::sockopt::curve_publickey,
                           ks.pubkey(cfg.keystore_name));
        ks.with_seckey(cfg.keystore_name,
            [&](std::string_view sec) {
                pImpl->dealer->set(zmq::sockopt::curve_secretkey, sec);
            });

        // I-DEALER-IDENTITY (HEP-CORE-0046 §8.1):
        //   the DEALER MUST set ZMQ_ROUTING_ID to its owning role's
        //   role_uid before connect().  Without this, libzmq assigns
        //   a rand()-derived 5-byte identity that collides across
        //   fresh processes (rand() with unseeded state returns the
        //   same first value in every process), so the broker's
        //   ROUTER cannot uniquely address distinct roles for
        //   unsolicited sends like CHANNEL_AUTH_CHANGED_NOTIFY.
        //
        // Empty role_uid is a broker-config error — refuse to connect
        // loud, matching the no-CURVE-without-config discipline above.
        if (cfg.role_uid.empty())
        {
            LOGGER_ERROR(
                "BrokerRequestComm: role_uid empty; refusing to connect "
                "(I-DEALER-IDENTITY §8.1 requires routing_id = role_uid).");
            pImpl->dealer.reset();
            return false;
        }
        pImpl->dealer->set(zmq::sockopt::routing_id, cfg.role_uid);

        // (Heartbeat 5s/30s + reconnect-disable + sndtimeo=500ms +
        // linger=0 are all applied above by `apply_socket_policy`,
        // uniform across every pylabhub ZMQ subsystem.  Per HEP-0023
        // §2.5.3 + IMPLEMENTATION_GUIDANCE.md
        // §"Role-side ZMQ socket policy".)

        pImpl->dealer->connect(cfg.broker_endpoint);
        pImpl->broker_endpoint = cfg.broker_endpoint;  // cache for log lines

        // Socket monitor for ZMQ_EVENT_DISCONNECTED.
        static std::atomic<int> s_monitor_id{0};
        pImpl->monitor_endpoint = fmt::format(
            "inproc://broker-req-monitor-{}",
            s_monitor_id.fetch_add(1, std::memory_order_relaxed));
        if (zmq_socket_monitor(pImpl->dealer->handle(),
                               pImpl->monitor_endpoint.c_str(),
                               ZMQ_EVENT_DISCONNECTED) == 0)
        {
            pImpl->monitor_sock.emplace(ctx, zmq::socket_type::pair);
            pImpl->monitor_sock->set(zmq::sockopt::linger, 0);
            pImpl->monitor_sock->connect(pImpl->monitor_endpoint);
        }

        // Inproc PAIR for command queue wake-up.
        static std::atomic<int> s_signal_id{0};
        std::string sig_addr = fmt::format(
            "inproc://broker-req-signal-{}",
            s_signal_id.fetch_add(1, std::memory_order_relaxed));
        pImpl->signal_read.emplace(ctx, zmq::socket_type::pair);
        pImpl->signal_write.emplace(ctx, zmq::socket_type::pair);
        pImpl->signal_read->set(zmq::sockopt::linger, 0);
        pImpl->signal_write->set(zmq::sockopt::linger, 0);
        pImpl->signal_read->bind(sig_addr);
        pImpl->signal_write->connect(sig_addr);

        // Wire MonitoredQueue push → signal socket.
        auto *sig_w = &(*pImpl->signal_write);
        pImpl->cmd_queue.set_on_push_signal([sig_w] {
            zmq::message_t wake("W", 1);
            try
            {
                sig_w->send(wake, zmq::send_flags::dontwait);
            }
            catch (const zmq::error_t &)
            {
                // Signal socket full — poll loop will drain on next cycle.
            }
        });

        pImpl->role_uid = cfg.role_uid;
        pImpl->role_name = cfg.role_name;
        pImpl->connected.store(true, std::memory_order_release);
        pImpl->stop_requested.store(false, std::memory_order_release);

        LOGGER_INFO("BrokerRequestComm: connected to {} (CurveZMQ)",
                    cfg.broker_endpoint);
        return true;
    }
    catch (const zmq::error_t &e)
    {
        LOGGER_ERROR("BrokerRequestComm: connect failed: {}", e.what());
        pImpl->monitor_sock.reset();
        pImpl->dealer.reset();
        pImpl->signal_read.reset();
        pImpl->signal_write.reset();
        return false;
    }
}

void BrokerRequestComm::disconnect()
{
    pImpl->stop_requested.store(true, std::memory_order_release);
    pImpl->cmd_queue.set_on_push_signal(nullptr);

    // Close monitor before dealer to avoid spurious hub-dead on voluntary disconnect.
    if (pImpl->monitor_sock)
    {
        if (pImpl->dealer)
            zmq_socket_monitor(pImpl->dealer->handle(), nullptr, 0);
        pImpl->monitor_sock.reset();
    }

    pImpl->signal_write.reset();
    pImpl->signal_read.reset();
    pImpl->dealer.reset();
    pImpl->connected.store(false, std::memory_order_release);
}

bool BrokerRequestComm::is_connected() const noexcept
{
    return pImpl->connected.load(std::memory_order_acquire);
}

bool BrokerRequestComm::reconnect_disabled() const noexcept
{
    // Audit S1 (2026-05-18) — pylabhub policy: disconnect is
    // terminal (HEP-CORE-0023 §2.5.3).  Read the live socket
    // option (rather than echoing a stored config value) so the
    // assertion is true END-TO-END including any libzmq quirks.
    if (!pImpl || !pImpl->dealer) return false;
    try {
        return pImpl->dealer->get(zmq::sockopt::reconnect_ivl) == -1;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Callbacks
// ============================================================================

void BrokerRequestComm::on_notification(NotificationCallback cb)
{
    pImpl->on_notification_cb = std::move(cb);
}

void BrokerRequestComm::on_hub_dead(std::function<void()> cb)
{
    pImpl->on_hub_dead_cb = std::move(cb);
}

// ============================================================================
// Periodic tasks
// ============================================================================

void BrokerRequestComm::set_periodic_task(std::function<void()> action,
                                              int interval_ms,
                                              std::function<uint64_t()> get_iteration)
{
    // All installs (pre-startup + post-startup, e.g. heartbeat scheduled
    // after REG_ACK heartbeat-cadence negotiation per HEP-CORE-0023 §2.5)
    // flow through the cmd queue.  The handler runs on the ctrl thread and
    // appends directly into the active poll-loop's periodic_tasks vector.
    pImpl->cmd_queue.push(InstallPeriodicTaskCmd{
        std::move(action), interval_ms, std::move(get_iteration)});
}

// ============================================================================
// Poll loop
// ============================================================================

void BrokerRequestComm::run_poll_loop(std::function<bool()> should_run)
{
    if (!pImpl->dealer)
    {
        LOGGER_ERROR("BrokerRequestComm: run_poll_loop called without connect");
        return;
    }

    scripting::ZmqPollLoop loop{
        [this, &should_run] {
            return should_run() &&
                   !pImpl->stop_requested.load(std::memory_order_acquire);
        },
        "broker"};

    // Poll the DEALER socket for incoming broker messages.  Also
    // opportunistically check the monitor here when DEALER traffic
    // wakes the poll — keeps the side-effect for the heartbeat-acks
    // path; the monitor-socket-as-poll-item below is the authoritative
    // hub-dead detector for idle periods (no DEALER traffic).
    loop.sockets.push_back(
        {zmq::socket_ref(zmq::from_handle, pImpl->dealer->handle()),
         [this] {
             pImpl->recv_and_dispatch();
             pImpl->check_monitor();
         }});

    // Poll the socket-monitor PAIR socket as a first-class poll item
    // so ZMQ_EVENT_DISCONNECTED is delivered to on_hub_dead promptly
    // even when the role is idle (no DEALER traffic).  Pre-A2 the
    // monitor was only drained as a DEALER side-effect — in
    // production heartbeats kept DEALER busy enough to mask this,
    // but role-side init code that connects and pauses (the A2 test
    // case) waited up to the ZMTP heartbeat_timeout (30s) before
    // observing a dead broker.  After this change the monitor PAIR
    // wakes the poll itself on disconnect.
    if (pImpl->monitor_sock)
    {
        loop.sockets.push_back(
            {zmq::socket_ref(zmq::from_handle, pImpl->monitor_sock->handle()),
             [this] { pImpl->check_monitor(); }});
    }

    // Signal socket for command queue wake-up.
    if (pImpl->signal_read)
    {
        loop.signal_socket =
            zmq::socket_ref(zmq::from_handle, pImpl->signal_read->handle());
        loop.drain_commands = [this] { pImpl->drain_command_queue(); };
    }

    // Publish a pointer to the loop's periodic_tasks vector so the
    // InstallPeriodicTaskCmd handler can append into it from inside the
    // drain handler.  The loop starts with an empty vector.  install_heartbeat
    // is invoked POST-REG_ACK at S3 per HEP-CORE-0036 §3.5.4 INV1 (HB cadence
    // starts only after the data plane is up).  The cmd-queue indirection is
    // the wakeup mechanism for posting tasks (including the install_heartbeat
    // call itself) from the role-host thread into this BRC poll loop; the
    // loop drains the cmd queue on every poll iteration.  See HEP-CORE-0023
    // §2.5 for the heartbeat-cadence negotiation that motivates this.
    pImpl->active_loop_periodic_tasks = &loop.periodic_tasks;

    loop.run();

    // Thread Shutdown Contract (HEP-CORE-0031 §4.1): once loop.run()
    // returns, this thread MUST NOT touch pImpl.  Any pImpl access here
    // would race against the teardown caller destroying broker_comm_
    // (pre-MD1 this site had two dead diagnostic stores —
    // `poll_loop_running.store(false)` and
    // `active_loop_periodic_tasks = nullptr` — that exposed the
    // gdb-captured use-after-free at line 594.  Both were removed; the
    // `active_loop_periodic_tasks` pointer's only consumer is
    // `handle_command` which only runs on this thread during
    // loop.run() — once the loop has returned, no further reader will
    // observe the pointer, dangling or not.)
    //
    // The spawn-site invokes `loop.run()` from inside a
    // `ctx.with_active_loop(...)` transactional bracket; RAII
    // decrements `active_loop_depth` when this function returns, which
    // is the signal the teardown caller waits on via
    // `wait_for_quiescence`.
}

size_t BrokerRequestComm::unmatched_replies() const noexcept
{
    return pImpl->unmatched_replies_count.load(std::memory_order_relaxed);
}

void BrokerRequestComm::stop() noexcept
{
    pImpl->stop_requested.store(true, std::memory_order_release);

    // Send a wake-up to break the poll.
    if (pImpl->signal_write)
    {
        try
        {
            pImpl->signal_write->send(zmq::message_t("S", 1),
                                      zmq::send_flags::dontwait);
        }
        catch (...)
        {
            // Best-effort wake-up — if the inproc PAIR send fails the
            // poll loop simply waits out its timeout instead of
            // bailing immediately.  Intentional silent swallow; not
            // load-bearing for shutdown correctness.  Logging here
            // would be noise (stop() can race with the poll loop's
            // own teardown of signal_read on context termination).
        }
    }
}

// ============================================================================
// Fire-and-forget messages
// ============================================================================

void BrokerRequestComm::send_heartbeat(const std::string &channel,
                                           const std::string &role_uid,
                                           const std::string &role_type,
                                           const nlohmann::json &metrics)
{
    // HEP-CORE-0019 §4.1 / HEP-CORE-0023 §2.5.2 / HEP-CORE-0033 §18
    // (Phase 6 per-presence wire format).  The `role_uid` and
    // `role_type` fields are required; the broker handler reads them
    // from the payload and resolves the matching `ProducerEntry` /
    // `ConsumerEntry` row in the channel's `producers[]` /
    // `consumers[]` list per HEP-CORE-0023 §2.1.1.
    // broker_proto 4→5 (audit R3.5b, 2026-05-19): the wire key was
    // renamed `uid` → `role_uid` for cross-message consistency with
    // REG_REQ / CONSUMER_REG_REQ / DEREG_REQ / ROLE_*_REQ.
    // `producer_pid` is retained from the Phase 1 wire format for
    // diagnostics; the broker uses it only for an ERROR log when
    // missing or zero.
    nlohmann::json payload;
    payload["channel_name"] = channel;
    payload["role_uid"]     = role_uid;
    payload["role_type"]    = role_type;
    payload["producer_pid"] = pylabhub::platform::get_pid();
    if (!metrics.empty())
        payload["metrics"] = metrics;
    SendCmd cmd;
    cmd.msg_type       = "HEARTBEAT_NOTIFY";
    cmd.correlation_id = make_random_hex16();
    cmd.payload        = std::move(payload);
    pImpl->cmd_queue.push(std::move(cmd));
}

// M1.4 (2026-05-11): `BrokerRequestComm::send_metrics_report` deleted.
// Metrics piggyback on `send_heartbeat(channel, uid, role_type, metrics)`
// per HEP-CORE-0019 §2.3 Phase 6.
//
// Audit O1 (2026-05-17): `BrokerRequestComm::send_notify` removed —
// zero callers in src/ + tests/ + examples/ as of 2026-05-17.  The
// matching broker-side handler `handle_channel_notify_req` is kept
// because HEP-CORE-0022 federation peers may still emit
// `CHANNEL_NOTIFY_REQ` on the wire (relay path).  See HEP-CORE-0030
// §9.1 for the channel-bound-vs-band-bound coexistence rationale.

void BrokerRequestComm::send_broadcast(const std::string &target,
                                            const std::string &sender_uid,
                                            const std::string &msg,
                                            const std::string &data)
{
    nlohmann::json payload;
    payload["target_channel"] = target;
    payload["sender_uid"] = sender_uid;
    payload["message"] = msg;
    payload["data"] = data;
    SendCmd cmd;
    cmd.msg_type       = "CHANNEL_BROADCAST_SEND_NOTIFY";
    cmd.correlation_id = make_random_hex16();
    cmd.payload        = std::move(payload);
    pImpl->cmd_queue.push(std::move(cmd));
}

void BrokerRequestComm::send_checksum_error(const nlohmann::json &report)
{
    SendCmd cmd;
    cmd.msg_type       = "CHECKSUM_ERROR_REPORT";
    cmd.correlation_id = make_random_hex16();
    cmd.payload        = report;
    pImpl->cmd_queue.push(std::move(cmd));
}

std::optional<nlohmann::json>
BrokerRequestComm::send_endpoint_update(const std::string &channel,
                                         const std::string &endpoint_type,
                                         const std::string &endpoint,
                                         int timeout_ms)
{
    // Sync REQ/REP per HEP-CORE-0007 §12.2.1 + HEP-CORE-0021 §16.3.
    // Broker handler (broker_service.cpp:handle_endpoint_update_req)
    // mutates `ProducerEntry.zmq_node_endpoint` before emitting
    // ENDPOINT_UPDATE_ACK — the ACK is therefore a durability barrier.
    // Pre-2026-05-21 this was `cmd_queue.push(...) → void`, dropping
    // the ACK and exposing a race where a consumer's DISC_REQ could
    // arrive at the broker before the producer's update had been
    // applied.  See header doc for the caller contract.
    nlohmann::json payload;
    payload["channel_name"]  = channel;
    payload["endpoint_type"] = endpoint_type;
    payload["endpoint"]      = endpoint;
    return pImpl->do_request("ENDPOINT_UPDATE_REQ", "ENDPOINT_UPDATE_ACK",
                              std::move(payload), timeout_ms);
}

// ============================================================================
// Request-reply methods
// ============================================================================

std::optional<nlohmann::json>
BrokerRequestComm::register_channel(const nlohmann::json &opts, int timeout_ms)
{
    return pImpl->do_request( "REG_REQ", "REG_ACK", opts, timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::get_channel_auth(const std::string &channel,
                                     const std::string &role_uid,
                                     int timeout_ms)
{
    nlohmann::json opts;
    opts["channel_name"] = channel;
    opts["role_uid"]     = role_uid;
    return pImpl->do_request("GET_CHANNEL_AUTH_REQ", "GET_CHANNEL_AUTH_ACK",
                             opts, timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::channel_auth_applied(const std::string &channel,
                                         const std::string &role_uid,
                                         std::string_view   role_type,
                                         std::uint64_t      applied_version,
                                         std::uint64_t      instance_id,
                                         int                timeout_ms)
{
    // HEP-CORE-0042 §5.5.2 wire.  Single wire; broker discriminates
    // producer vs consumer branch on `role_type` per HEP-CORE-0036
    // §I9.1.  `role_uid` populates BOTH `role_uid` (amended field)
    // AND `producer_role_uid` (back-compat field for the producer
    // branch) — broker's `handle_channel_auth_applied_req` reads
    // `role_uid` when present and falls back to `producer_role_uid`
    // otherwise, so writing both keeps a single caller shape and
    // preserves the ack echo semantics for any older broker that
    // hasn't taken the §5.5.2 amendment.
    nlohmann::json opts;
    opts["channel_name"]      = channel;
    opts["role_uid"]          = role_uid;
    opts["producer_role_uid"] = role_uid;
    opts["role_type"]         = std::string(role_type);
    opts["applied_version"]   = applied_version;
    opts["instance_id"]       = instance_id;
    auto reply = pImpl->do_request("CHANNEL_AUTH_APPLIED_REQ",
                                    "CHANNEL_AUTH_APPLIED_ACK",
                                    opts, timeout_ms);
    // HEP-CORE-0042 Phase 3 review-B fix (2026-07-02) — DEFENSIVE
    // reply-content verification (see consumer_attach_zmq for full
    // rationale).  If the broker's `CHANNEL_AUTH_APPLIED_ACK` echoes
    // a channel_name that doesn't match our request, treat as timeout
    // — likely a cross-wire from a concurrent APPLIED_REQ for a
    // different channel that got serialized through the same BRC
    // pending_requests slot.
    if (reply.has_value())
    {
        const auto echoed_channel =
            reply->value("channel_name", std::string{});
        if (!echoed_channel.empty() && echoed_channel != channel)
        {
            LOGGER_WARN(
                "BrokerRequestComm::channel_auth_applied('{}', role_type="
                "'{}'): reply echoed channel_name='{}' — cross-wire "
                "suspected; treating as no-reply (broker will re-drive "
                "via next NOTIFY per §5.5.2)",
                channel, role_type, echoed_channel);
            return std::nullopt;
        }
    }
    return reply;
}

std::optional<nlohmann::json>
BrokerRequestComm::check_peer_ready(const std::string &channel,
                                     const std::string &role_uid,
                                     const std::string &pubkey_z85,
                                     int                timeout_ms)
{
    // HEP-CORE-0036 §6.6.3 wire.  Dialing-side role's readiness pull.
    nlohmann::json opts;
    opts["channel_name"] = channel;
    opts["role_uid"]     = role_uid;
    opts["pubkey_z85"]   = pubkey_z85;
    auto reply = pImpl->do_request("CHECK_PEER_READY_REQ",
                                    "CHECK_PEER_READY_ACK",
                                    opts, timeout_ms);
    if (reply.has_value())
    {
        const auto echoed_channel =
            reply->value("channel_name", std::string{});
        if (!echoed_channel.empty() && echoed_channel != channel)
        {
            LOGGER_WARN(
                "BrokerRequestComm::check_peer_ready('{}'): reply echoed "
                "channel_name='{}' — cross-wire suspected; treating as "
                "no-reply",
                channel, echoed_channel);
            return std::nullopt;
        }
    }
    return reply;
}

std::optional<nlohmann::json>
BrokerRequestComm::consumer_attach_zmq(const std::string &channel,
                                        const std::string &consumer_role_uid,
                                        const std::string &consumer_pubkey,
                                        const std::string &producer_role_uid,
                                        int                timeout_ms)
{
    // HEP-CORE-0042 §5.5.1 wire shape.  `role_uid` on the wire is a
    // sibling-BRC-method naming convention (`get_channel_auth`,
    // `channel_auth_applied`, `consumer_attach` for SHM all use the
    // same key) — historic identifier the broker's dispatcher keys
    // on for producer resolution.  Body-shape parity with the SHM
    // sibling helps future §6.2.1 wire-format audits catch drift
    // between the two per-transport branches.
    nlohmann::json opts;
    opts["channel_name"]      = channel;
    opts["consumer_role_uid"] = consumer_role_uid;
    opts["consumer_pubkey"]   = consumer_pubkey;
    opts["producer_role_uid"] = producer_role_uid;
    auto reply = pImpl->do_request("CONSUMER_ATTACH_REQ_ZMQ",
                                    "CONSUMER_ATTACH_ACK_ZMQ",
                                    opts, timeout_ms);
    // HEP-CORE-0042 Phase 3 review-B fix (2026-07-02) — DEFENSIVE
    // reply-content verification.  BRC's `pending_requests` map keys
    // by msg_type only; under fan-in (§7.1 loop calls this method
    // serially for N producers), if iter N times out client-side and
    // a delayed CONSUMER_ATTACH_ACK_ZMQ for producer N arrives after
    // iter N+1 has registered a new request under the same ack type,
    // the delayed reply cross-wires into iter N+1's waiter.  The
    // `abandoned` flag on RequestCmd protects a narrow window before
    // the map overwrite; this check catches the post-overwrite case.
    // If the reply's `producer_role_uid` echo doesn't match what we
    // asked about, treat as timeout (log WARN + return nullopt) so
    // the §7.1 loop synthesizes the standard timeout reason.  Proper
    // BRC-level fix (per-request correlation_id keying) is tracked
    // as follow-up.
    if (reply.has_value())
    {
        const auto echoed_uid =
            reply->value("producer_role_uid", std::string{});
        if (echoed_uid != producer_role_uid)
        {
            LOGGER_WARN(
                "BrokerRequestComm::consumer_attach_zmq('{}','{}'): reply "
                "echoed producer_role_uid='{}' but expected '{}' — likely "
                "fan-in cross-wire (BRC msg_type-only keying + slow "
                "broker reply after client-side timeout).  Treating as "
                "timeout; §7.1 loop will synthesize a §5.6 timeout "
                "reason.",
                channel, producer_role_uid, echoed_uid, producer_role_uid);
            return std::nullopt;
        }
    }
    return reply;
}

std::optional<nlohmann::json>
BrokerRequestComm::consumer_attach(const std::string &channel,
                                   const std::string &consumer_pubkey,
                                   const std::string &consumer_role_uid,
                                   const std::string &producer_role_uid,
                                   int                timeout_ms)
{
    nlohmann::json opts;
    opts["channel_name"]      = channel;
    opts["consumer_pubkey"]   = consumer_pubkey;
    opts["consumer_role_uid"] = consumer_role_uid;
    opts["role_uid"]          = producer_role_uid;
    return pImpl->do_request("CONSUMER_ATTACH_REQ_SHM", "CONSUMER_ATTACH_ACK_SHM",
                             opts, timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::discover_channel(const std::string &channel,
                                        const nlohmann::json &opts,
                                        int timeout_ms)
{
    // HEP-CORE-0023 §2.2: DISC_REQ may reply with DISC_ACK (Ready),
    // DISC_PENDING (retry), or ERROR (CHANNEL_NOT_FOUND). Loop on PENDING
    // until Ready or overall timeout; ERROR/CHANNEL_NOT_FOUND: retry briefly
    // (producer may register later) within our timeout budget.
    constexpr int kRetryIntervalMs = 100;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    while (true)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            LOGGER_WARN("BrokerRequestComm: discover_channel('{}') overall timeout ({}ms)",
                        channel, timeout_ms);
            return std::nullopt;
        }
        const int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        const int per_attempt_ms = std::min(remaining_ms, 2000);

        nlohmann::json payload = opts;
        payload["channel_name"] = channel;

        auto result = pImpl->do_request_multi(
            "DISC_REQ", {"DISC_ACK", "DISC_PENDING"},
            std::move(payload), per_attempt_ms);

        if (!result.has_value())
        {
            // ERROR (CHANNEL_NOT_FOUND) or per-attempt timeout.
            // Retry: producer may register later.
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            continue;
        }

        const std::string status = result->value("status", "");
        if (status == "success")
            return result;      // DISC_ACK — channel Ready

        if (status == "pending")
        {
            // DISC_PENDING — producer registered but not yet Ready.
            LOGGER_TRACE("BrokerRequestComm: DISC_PENDING for '{}', retrying", channel);
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            continue;
        }

        // Unexpected status — treat as failure.
        LOGGER_WARN("BrokerRequestComm: DISC_REQ unexpected status='{}' for '{}'",
                    status, channel);
        return std::nullopt;
    }
}

std::optional<nlohmann::json>
BrokerRequestComm::register_consumer(const nlohmann::json &opts, int timeout_ms)
{
    return pImpl->do_request( "CONSUMER_REG_REQ", "CONSUMER_REG_ACK", opts, timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::deregister_channel(const std::string &channel, int timeout_ms)
{
    // broker_proto 2→3: `role_uid` REQUIRED on the wire so the broker
    // can resolve the target producer by (pid, uid) tuple instead of
    // pid-alone.  HEP-CORE-0023 §2.1.1 multi-producer channels admit
    // multiple producers; PID-only resolution was racy under OS pid
    // reuse across role restarts.
    nlohmann::json payload;
    payload["channel_name"] = channel;
    payload["role_uid"]     = pImpl->role_uid;
    payload["producer_pid"] = static_cast<uint64_t>(::getpid());
    return pImpl->do_request("DEREG_REQ", "DEREG_ACK",
                             std::move(payload), timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::deregister_consumer(const std::string &channel, int timeout_ms)
{
    // broker_proto 2→3: same as DEREG_REQ — `role_uid` REQUIRED on the
    // wire so the broker resolves the target consumer by (pid, uid)
    // tuple.  Closes the analogous race for multi-consumer channels.
    nlohmann::json payload;
    payload["channel_name"] = channel;
    payload["role_uid"]     = pImpl->role_uid;
    payload["consumer_pid"] = static_cast<uint64_t>(::getpid());
    return pImpl->do_request("CONSUMER_DEREG_REQ", "CONSUMER_DEREG_ACK",
                             std::move(payload), timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::query_role_presence(const std::string &uid, int timeout_ms)
{
    // HEP-CORE-0007 §"ROLE_PRESENCE_REQ" — wire field `role_uid`
    // (unified with REG_REQ / CONSUMER_REG_REQ; old `uid` form retired
    // 2026-05-09 as part of the protocol-doc-vs-code unification).
    nlohmann::json payload;
    payload["role_uid"] = uid;
    return pImpl->do_request("ROLE_PRESENCE_REQ", "ROLE_PRESENCE_ACK",
                             std::move(payload), timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::query_role_info(const std::string &uid, int timeout_ms)
{
    // HEP-CORE-0007 §"ROLE_INFO_REQ" — wire field `role_uid`
    // (unified with REG_REQ / CONSUMER_REG_REQ / ROLE_PRESENCE_REQ;
    // old `uid` form retired 2026-05-09).
    nlohmann::json payload;
    payload["role_uid"] = uid;
    return pImpl->do_request( "ROLE_INFO_REQ", "ROLE_INFO_ACK",
                      std::move(payload), timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::list_channels(int timeout_ms)
{
    return pImpl->do_request("CHANNEL_LIST_REQ", "CHANNEL_LIST_ACK",
                             nlohmann::json::object(), timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::query_shm_info(const std::string &channel, int timeout_ms)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    return pImpl->do_request( "SHM_BLOCK_QUERY_REQ", "SHM_BLOCK_QUERY_ACK",
                      std::move(payload), timeout_ms);
}

// ============================================================================
// Band pub/sub messaging (HEP-CORE-0030)
// ============================================================================
// Wire payload key is `band` per HEP-CORE-0030 §5.1.  The 2026-04-11
// rename refactor (`8d3ee1e`) renamed C++ classes, file names, API
// methods, and wire message types from "channel pub/sub" to "band" but
// missed the payload key strings — the audit-B1 sweep (2026-05-17)
// completes that rename on the wire.  Broker proto bumped 3→4 to
// signal the break.

std::optional<nlohmann::json>
BrokerRequestComm::band_join(const std::string &band, int timeout_ms)
{
    nlohmann::json payload;
    payload["band"] = band;
    payload["role_uid"] = pImpl->role_uid;
    payload["role_name"] = pImpl->role_name;
    return pImpl->do_request("BAND_JOIN_REQ", "BAND_JOIN_ACK",
                             std::move(payload), timeout_ms);
}

std::optional<nlohmann::json>
BrokerRequestComm::band_leave(const std::string &band, int timeout_ms)
{
    nlohmann::json payload;
    payload["band"] = band;
    payload["role_uid"] = pImpl->role_uid;
    return pImpl->do_request("BAND_LEAVE_REQ", "BAND_LEAVE_ACK",
                             std::move(payload), timeout_ms);
}

void BrokerRequestComm::band_broadcast(const std::string &band,
                                           const nlohmann::json &body)
{
    // broker_proto 4→5 (audit R3.5b, 2026-05-19): sender wire key
    // renamed `sender_uid` → `role_uid` — uniform with all other
    // role-context gates.  Federation peer-context `sender_uid` (used
    // for HUB_TARGETED_MSG / CHANNEL_BROADCAST_SEND_NOTIFY from hubs) carries
    // a peer.uid not a role.uid and keeps its name.
    nlohmann::json payload;
    payload["band"] = band;
    payload["role_uid"] = pImpl->role_uid;
    payload["body"] = body;
    SendCmd cmd;
    cmd.msg_type       = "BAND_BROADCAST_SEND_NOTIFY";
    cmd.correlation_id = make_random_hex16();
    cmd.payload        = std::move(payload);
    pImpl->cmd_queue.push(std::move(cmd));
}

std::optional<nlohmann::json>
BrokerRequestComm::band_members(const std::string &band,
                                    int timeout_ms)
{
    nlohmann::json payload;
    payload["band"] = band;
    return pImpl->do_request("BAND_MEMBERS_REQ", "BAND_MEMBERS_ACK",
                             std::move(payload), timeout_ms);
}

} // namespace pylabhub::hub
