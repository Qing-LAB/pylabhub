/**
 * @file admin_service.cpp
 * @brief AdminService — REP socket + JSON envelope dispatcher (HEP-0033 §11).
 *
 * Phase 6.2a skeleton.  Methods return `not_implemented` placeholders
 * except for the built-in `ping` which proves the round-trip.  Phases
 * 6.2b/6.2c fill in the §11.2 query / control method bodies.
 *
 * Layer placement: this file lives in `src/utils/ipc/` (alongside
 * `broker_service.cpp` — both are REP-socket-owning hub subsystems)
 * rather than `src/utils/service/` as the original HEP §11.4 said.
 * Updated HEP doc accompanies this commit.
 */

#include "utils/admin_service.hpp"

#include "utils/broker_service.hpp"
#include "utils/config/hub_admin_config.hpp"
#include "utils/curve_socket.hpp"          // arm_curve_server (shared CURVE-server arm)
#include "utils/hub_api.hpp"               // augment_* hooks (HEP-0033 §12.2.2)
#include "utils/hub_host.hpp"
#include "utils/hub_metrics_filter.hpp"
#include "utils/hub_state.hpp"
#include "utils/hub_state_json.hpp"
#include "utils/logger.hpp"
#include "utils/admin_session.hpp"          // sealed session identity (§11.0.5)
#include "utils/security/key_store.hpp"        // kHubIdentityName, secure().keys()
#include "utils/security/secure_subsystem.hpp" // secure()
#include "utils/timeout_constants.hpp"
#include "utils/wire_bodies.hpp"            // typed admin bodies + ADMIN_* msg_types
#include "utils/wire_envelope.hpp"          // typed ROUTER envelope

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"             // zmq::multipart_t

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace pylabhub::admin
{

using nlohmann::json;

namespace
{

/// Build an error response in the canonical envelope shape.
json make_error(std::string_view code, std::string_view message)
{
    return json{
        {"status", "error"},
        {"error",  {{"code", std::string(code)}, {"message", std::string(message)}}},
    };
}

/// Build a successful response in the canonical envelope shape.
json make_ok(json result)
{
    return json{
        {"status", "ok"},
        {"result", std::move(result)},
    };
}

} // namespace

// ============================================================================
// Impl
// ============================================================================

struct AdminService::Impl
{
    zmq::context_t                &ctx;
    hub_host::HubHost             &host;
    std::string                    endpoint;
    std::string                    admin_token;

    std::atomic<bool>              stop_requested{false};
    std::string                    bound_endpoint;

    Impl(zmq::context_t &c, hub_host::HubHost &h)
        : ctx(c), host(h)
    {}

    /// Typed dispatcher — routes a parsed admin envelope by msg_type,
    /// verifies the session (except establishment) against the observed
    /// connection facts (§11.0.5), runs the (unchanged) `handle_*` member
    /// with the params it expects, and returns {ack_msg_type, ack_body}.
    /// Any failure becomes an `ADMIN_ERROR` reply.
    std::pair<std::string, nlohmann::json>
    dispatch_typed(const wire::WireEnvelope &env,
                   std::string_view          peer_address,
                   std::string_view          routing_id);

    /// Constant-time-equal-length admin token check (§11.3).
    [[nodiscard]] bool token_ok(std::string_view token) const noexcept;

    // ── Per-method handlers (HEP-CORE-0033 §11.2) ──────────────────────────
    // Each handler is invoked AFTER `dispatch` has validated the request
    // envelope and (when applicable) the admin token.  Handlers either
    // return a populated `make_ok(result)` envelope or one of the
    // typed `make_error(code, message)` envelopes per §11.5.
    //
    // No handler emits LOGGER_* on the success path — the audit Class D
    // gate forbids stray log noise on happy paths.  Handlers may emit
    // LOGGER_WARN / LOGGER_ERROR on legitimate error paths; the
    // accompanying L2 test must declare those via `ExpectLogWarn` etc.
    json handle_ping             (const json &request);
    json handle_list_channels    (const json &request);
    json handle_get_channel      (const json &request);
    json handle_list_roles       (const json &request);
    json handle_get_role         (const json &request);
    json handle_list_bands       (const json &request);
    json handle_list_peers       (const json &request);
    json handle_query_metrics    (const json &request);
    json handle_close_channel    (const json &request);
    json handle_broadcast_channel(const json &request);
    json handle_request_shutdown (const json &request);
};

// ============================================================================
// Construction / destruction
// ============================================================================

AdminService::AdminService(zmq::context_t          &zmq_ctx,
                            const config::HubAdminConfig &cfg,
                            std::string_view         admin_token,
                            hub_host::HubHost       &host)
    : impl_(std::make_unique<Impl>(zmq_ctx, host))
{
    impl_->endpoint       = cfg.endpoint;
    impl_->admin_token    = std::string(admin_token);

    // §11.3 invariant: the admin token is MANDATORY — there is no
    // token-less admin path (the CURVE transport in run() encrypts it, so
    // a non-loopback bind is no longer a hazard, but the token is still
    // the sole authority).  An empty token would silently authenticate
    // every request.
    if (admin_token.empty())
    {
        throw std::invalid_argument(
            "AdminService: admin_token is empty — the admin token is "
            "mandatory (HEP-CORE-0033 §11.3).  Vault not unlocked? "
            "HubConfig::load_keypair must run before AdminService "
            "construction.");
    }
}

AdminService::~AdminService() = default;

// ============================================================================
// Run / stop
// ============================================================================

void AdminService::run()
{
    zmq::socket_t sock(impl_->ctx, zmq::socket_type::router);

    // Allow rebind in tests where the same endpoint may be reused
    // after a fast teardown.  Matches the pattern broker_service uses.
    sock.set(zmq::sockopt::linger, 0);

    // ── CURVE-server arm (HEP-CORE-0033 §11.1) ────────────────────────────
    // The admin ROUTER is a curve_server keyed with the hub's EXISTING broker
    // identity keypair (kHubIdentityName) — the same server identity every
    // role already trusts; no separate admin key.  It authenticates the server
    // to the operator and ENCRYPTS the exchange, so the admin token (once, at
    // establishment) and the sealed session id (§11.0.5) never cross the wire
    // in cleartext.  Deliberately NO zap_domain: admin authority is the session
    // id, not a key allowlist; zap_enforce_domain=1 short-circuits ZAP so the
    // socket authenticates by CURVE crypto alone and stays off the broker's
    // single inproc ZAP pumper (HEP-CORE-0036 §7.4 single-pumper invariant).  The arm is the
    // shared helper (use-not-export), the same one the broker/inbox use.
    pylabhub::utils::arm_curve_server(
        sock, pylabhub::utils::security::kHubIdentityName);
    sock.set(zmq::sockopt::zap_enforce_domain, 1);

    sock.bind(impl_->endpoint);
    impl_->bound_endpoint = sock.get(zmq::sockopt::last_endpoint);

    // Per-instance session-sealing key (§11.0.5) — ephemeral to this run;
    // a restart mints a fresh key, invalidating every outstanding session id.
    ensure_session_seal_key();

    LOGGER_INFO("[admin] AdminService (typed console, ROUTER) listening on {} "
                "(CURVE-server, session-gated per HEP-CORE-0033 §11)",
                impl_->bound_endpoint);

    while (!impl_->stop_requested.load(std::memory_order_acquire))
    {
        zmq::pollitem_t items[] = {{static_cast<void *>(sock), 0, ZMQ_POLLIN, 0}};
        const int rc = zmq::poll(items, 1, std::chrono::milliseconds(kAdminPollIntervalMs));
        if (rc <= 0)
            continue;

        zmq::multipart_t raw;
        if (!raw.recv(sock))
            continue;

        // Hub-observed peer address from the routing-id frame, captured
        // BEFORE parse consumes the multipart (§11.0.5 anti-hijack fact).
        std::string peer_address;
        try
        {
            if (!raw.empty())
                peer_address = raw.at(0).gets("Peer-Address");
        }
        catch (const std::exception &)
        {
            // Non-TCP transport surfaces no Peer-Address; the routing id +
            // CURVE connection binding still gate the session.
        }

        wire::ParseError perr{};
        auto env = wire::WireEnvelope::parse_router_recv(std::move(raw), &perr);
        if (!env)
        {
            LOGGER_WARN("[admin] dropped malformed admin envelope (parse error {})",
                        static_cast<int>(perr));
            continue;
        }

        const std::string routing_id(env->identity());
        const std::string corr(env->correlation_id());

        std::string    ack_type;
        json           ack_body;
        try
        {
            std::tie(ack_type, ack_body) =
                impl_->dispatch_typed(*env, peer_address, routing_id);
        }
        catch (const wire::WireBodyError &e)
        {
            ack_type = std::string(wire::kAdminError);
            ack_body = json{{"code", "invalid_request"}, {"message", e.what()}};
        }
        catch (const std::exception &e)
        {
            ack_type = std::string(wire::kAdminError);
            ack_body = json{{"code", "internal"}, {"message", e.what()}};
        }

        // Route the reply back to the sender's DEALER; correlation_id echoes
        // the request (ACK / ERROR are not _NOTIFY, so it is non-empty —
        // parse_router_recv already enforced that policy on the request).
        auto reply = wire::WireEnvelope::build_router_send(
            routing_id, ack_type, corr, std::move(ack_body));
        reply.send(sock);
    }

    sock.close();
    LOGGER_INFO("[admin] AdminService stopped (was bound to {})",
                impl_->bound_endpoint);
}

void AdminService::stop() noexcept
{
    impl_->stop_requested.store(true, std::memory_order_release);
}

const std::string &AdminService::bound_endpoint() const noexcept
{
    return impl_->bound_endpoint;
}

// ============================================================================
// dispatch — envelope check, token gate, and method routing
// ============================================================================
//
// Method routing uses a static table keyed on the §11.2 method name.
// 11 entries today (1 ping + 7 query + 3 control); deferred methods
// are listed separately so an unknown name is `unknown_method` rather
// than `not_implemented`.

bool AdminService::Impl::token_ok(std::string_view token) const noexcept
{
    // §11.3 — the admin token authorizes session establishment (checked
    // once, at HELLO).  Size-gated constant-time-equal-length compare; the
    // vault token is fixed-length (64 hex).
    if (token.size() != admin_token.size())
        return false;
    bool eq = true;
    for (std::size_t i = 0; i < token.size(); ++i)
        eq &= (token[i] == admin_token[i]);
    return eq;
}

namespace
{

/// Hub wall clock in ms — folded into the session id (§11.0.5).
std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/// {ADMIN_ERROR, {code, message}} — the typed failure reply (§11.5).
std::pair<std::string, json> err_reply(std::string_view code,
                                       std::string_view message)
{
    return {std::string(wire::kAdminError),
            json{{"code", std::string(code)}, {"message", std::string(message)}}};
}

/// Convert an unchanged handler's `{status, result|error}` envelope into the
/// typed reply.  `result_ack` = query (wrap `result` object) vs control
/// (emit `{status:"ok"}`).  A handler error becomes an ADMIN_ERROR reply.
std::pair<std::string, json> to_reply(std::string_view ack_type,
                                      const json &handler_out, bool result_ack)
{
    if (handler_out.value("status", std::string{}) != "ok")
    {
        const json e = handler_out.value("error", json::object());
        return err_reply(e.value("code", "internal"), e.value("message", ""));
    }
    if (result_ack)
        return {std::string(ack_type),
                json{{"result", handler_out.value("result", json::object())}}};
    return {std::string(ack_type), json{{"status", "ok"}}};
}

} // namespace

std::pair<std::string, json>
AdminService::Impl::dispatch_typed(const wire::WireEnvelope &env,
                                   std::string_view          peer_address,
                                   std::string_view          routing_id)
{
    namespace w = pylabhub::wire;
    const std::string     mt(env.msg_type());
    const nlohmann::json &body = env.body();

    // ── Establishment (§11.0.5) — token authorizes; mint the session id ──
    if (mt == w::kAdminHelloReq)
    {
        w::AdminHelloReqBody b(body);
        if (!token_ok(b.token()))
            return err_reply("unauthorized", "invalid admin token");
        const AdminSessionFacts facts{b.label(), std::string(peer_address),
                                      std::string(routing_id), now_ms()};
        return {std::string(w::kAdminHelloAck),
                json{{"session_id", seal_session_id(facts)}}};
    }

    // Session + in-session-replay gate (§11.0.5).  Every command body carries
    // the session id AND the replay pair (client_nonce, client_wall_ts,
    // required by `require_security_triple`).  The gate: (1) verifies the
    // session id against the observed connection facts; (2) rejects wall-clock
    // skew; (3) rejects a reused nonce within the window — the per-session
    // nonce dedup reuses the SAME hub-wide store the REG plane uses, reached
    // here through `HubHost::nonce_seen` (the admin plane holds no mutable
    // HubState), keyed by the session's `origin_uid`.  Returns the typed
    // error reply on any failure, or nullopt to proceed.
    constexpr std::uint64_t kReplaySkewMs   = 30'000; // I-REPLAY-BOUND parity
    constexpr std::uint64_t kReplayWindowMs = 30'000; // MUST be >= skew
    const auto gate =
        [&](std::string_view sid) -> std::optional<std::pair<std::string, json>> {
        auto facts = verify_session_id(sid, peer_address, routing_id);
        if (!facts)
            return err_reply("unauthorized", "invalid or hijacked session");
        const std::uint64_t ts   = body.value("client_wall_ts", std::uint64_t{0});
        const std::uint64_t now  = now_ms();
        const std::uint64_t skew = now > ts ? now - ts : ts - now;
        if (skew > kReplaySkewMs)
            return err_reply("replay_or_skew", "admin command wall-clock skew "
                                               "exceeds tolerance");
        const std::string origin = origin_uid(*facts);
        const std::string nonce  = body.value("client_nonce", std::string{});
        if (!host.nonce_seen(origin, nonce, ts, kReplayWindowMs))
            return err_reply("replay_or_skew",
                             "client_nonce reused (in-session replay)");
        return std::nullopt;
    };

    // ── Query methods → AdminResultAck ──
    if (mt == w::kAdminPingReq)
    {
        w::AdminPingReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        return to_reply(w::kAdminPingAck, handle_ping(req), false);
    }
    if (mt == w::kAdminListChannelsReq)
    {
        w::AdminSessionReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        return to_reply(w::kAdminListChannelsAck, handle_list_channels(req), true);
    }
    if (mt == w::kAdminListRolesReq)
    {
        w::AdminSessionReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        return to_reply(w::kAdminListRolesAck, handle_list_roles(req), true);
    }
    if (mt == w::kAdminListBandsReq)
    {
        w::AdminSessionReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        return to_reply(w::kAdminListBandsAck, handle_list_bands(req), true);
    }
    if (mt == w::kAdminListPeersReq)
    {
        w::AdminSessionReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        return to_reply(w::kAdminListPeersAck, handle_list_peers(req), true);
    }
    if (mt == w::kAdminGetChannelReq)
    {
        w::AdminNamedReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        req["params"]["channel"] = b.name();
        return to_reply(w::kAdminGetChannelAck, handle_get_channel(req), true);
    }
    if (mt == w::kAdminGetRoleReq)
    {
        w::AdminNamedReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        req["params"]["uid"] = b.name();
        return to_reply(w::kAdminGetRoleAck, handle_get_role(req), true);
    }
    if (mt == w::kAdminQueryMetricsReq)
    {
        w::AdminQueryMetricsReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        req["params"] = b.filter();
        return to_reply(w::kAdminQueryMetricsAck, handle_query_metrics(req), true);
    }

    // ── Control methods → AdminStatusAck (accepted, §11.0.4 fire-and-forget) ──
    if (mt == w::kAdminCloseChannelReq)
    {
        w::AdminCloseChannelReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        req["params"]["channel"] = b.channel();
        return to_reply(w::kAdminCloseChannelAck, handle_close_channel(req), false);
    }
    if (mt == w::kAdminBroadcastChannelReq)
    {
        w::AdminBroadcastChannelReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        req["params"]["channel"] = b.channel();
        req["params"]["message"] = b.message();
        req["params"]["data"]    = b.data();
        return to_reply(w::kAdminBroadcastChannelAck, handle_broadcast_channel(req), false);
    }
    if (mt == w::kAdminRequestShutdownReq)
    {
        w::AdminSessionReqBody b(body);
        if (auto rej = gate(b.session_id())) return *rej;
        json req;
        return to_reply(w::kAdminRequestShutdownAck, handle_request_shutdown(req), false);
    }

    return err_reply("unknown_method",
                     std::string("unrecognised admin msg_type '") + mt + "'");
}

// ============================================================================
// Per-method handlers — Phase 6.2a (ping)
// ============================================================================

json AdminService::Impl::handle_ping(const json &request)
{
    // Built-in round-trip probe.  Echoes optional `params` back as
    // the result so tests can assert end-to-end serialization
    // works in both directions.
    json result = json::object();
    result["pong"] = true;
    if (request.contains("params"))
        result["echo"] = request["params"];
    return make_ok(std::move(result));
}

// ============================================================================
// Per-method handlers — Phase 6.2b (query block — HEP-0033 §11.2)
// ============================================================================
//
// All read-through `host.state()` (HubState) accessors.  No mutation;
// the broker keeps its single-mutator invariant.  Per HEP §11.5:
//   - `not_found` for a `get_*` target that does not exist
//   - `invalid_params` for a missing/wrong-type request field
//
// `host.state()` is safe to call before HubHost::startup() (HubState
// is a value member); the snapshot will simply be empty.

json AdminService::Impl::handle_list_channels(const json & /*request*/)
{
    const auto snap = host.state().snapshot();
    json channels = json::object();
    for (const auto &[name, ch] : snap.channels)
    {
        const auto obs = pylabhub::hub::observe_channel(ch, snap);
        channels[name] = pylabhub::hub::channel_to_json(ch, obs);
    }
    return make_ok(std::move(channels));
}

json AdminService::Impl::handle_get_channel(const json &request)
{
    const auto pit = request.find("params");
    if (pit == request.end() || !pit->is_object() ||
        !pit->contains("channel") || !(*pit)["channel"].is_string())
        return make_error("invalid_params",
                          "get_channel requires params.channel (string)");
    const auto &name = (*pit)["channel"].get_ref<const std::string &>();
    // Single snapshot keeps the channel + producer-presence lookup
    // consistent.  HEP-CORE-0023 §2.2 derives `observable` by scanning
    // every producer-presence row keyed under each
    // `ChannelEntry.producers[i].role_uid` and returning the
    // "best of all producers" observable (multi-producer, §2.1.1).
    const auto snap = host.state().snapshot();
    const auto cit  = snap.channels.find(name);
    if (cit == snap.channels.end())
        return make_error("not_found",
                          std::string("channel '") + name + "' not registered");
    const auto obs = pylabhub::hub::observe_channel(cit->second, snap);
    json result    = pylabhub::hub::channel_to_json(cit->second, obs);
    // HEP-CORE-0033 §12.2.2 — script-side response augmentation hook.
    // No-op when no script is loaded or the script doesn't define
    // `on_get_channel`; otherwise the script may mutate `result`.
    if (auto *api = host.hub_api())
        api->augment_get_channel(name, result);
    return make_ok(std::move(result));
}

json AdminService::Impl::handle_list_roles(const json & /*request*/)
{
    const auto snap = host.state().snapshot();
    json roles = json::object();
    for (const auto &[uid, r] : snap.roles)
        roles[uid] = pylabhub::hub::role_to_json(r);
    if (auto *api = host.hub_api())
        api->augment_list_roles(roles);
    return make_ok(std::move(roles));
}

json AdminService::Impl::handle_get_role(const json &request)
{
    const auto pit = request.find("params");
    if (pit == request.end() || !pit->is_object() ||
        !pit->contains("uid") || !(*pit)["uid"].is_string())
        return make_error("invalid_params",
                          "get_role requires params.uid (string)");
    const auto &uid = (*pit)["uid"].get_ref<const std::string &>();
    auto r = host.state().role(uid);
    if (!r)
        return make_error("not_found",
                          std::string("role '") + uid + "' not registered");
    return make_ok(pylabhub::hub::role_to_json(*r));
}

json AdminService::Impl::handle_list_bands(const json & /*request*/)
{
    const auto snap = host.state().snapshot();
    json bands = json::object();
    for (const auto &[name, b] : snap.bands)
        bands[name] = pylabhub::hub::band_to_json(b);
    return make_ok(std::move(bands));
}

json AdminService::Impl::handle_list_peers(const json & /*request*/)
{
    const auto snap = host.state().snapshot();
    json peers = json::object();
    for (const auto &[uid, p] : snap.peers)
        peers[uid] = pylabhub::hub::peer_to_json(p);
    return make_ok(std::move(peers));
}

json AdminService::Impl::handle_query_metrics(const json &request)
{
    // Optional `params` filter — narrows the response to specific
    // categories / channels / roles / bands / peers.  Empty params
    // (or empty filter fields) means include everything.
    //
    // Delegates to the broker's existing metrics aggregator
    // (HEP-0019 + HEP-0033 §9.4) so admin RPC and the legacy query
    // path emit identical JSON.  The broker snapshots HubState
    // under its own lock before reading — we don't need to.
    pylabhub::hub::MetricsFilter filter;
    const auto pit = request.find("params");
    if (pit != request.end() && pit->is_object())
    {
        // Per-field type validation.  Anything wrong is invalid_params
        // (a typo'd filter should not silently include everything).
        auto opt_string_array = [&](const char *field,
                                    std::vector<std::string> &out) -> json
        {
            const auto it = pit->find(field);
            if (it == pit->end()) return {};
            if (!it->is_array())
                return make_error("invalid_params",
                    std::string("query_metrics: params.") + field +
                    " must be an array of strings");
            for (const auto &v : *it)
            {
                if (!v.is_string())
                    return make_error("invalid_params",
                        std::string("query_metrics: params.") + field +
                        " entries must be strings");
                out.push_back(v.get<std::string>());
            }
            return {};
        };
        if (auto e = opt_string_array("channels", filter.channels); !e.is_null()) return e;
        if (auto e = opt_string_array("roles",    filter.roles);    !e.is_null()) return e;
        if (auto e = opt_string_array("bands",    filter.bands);    !e.is_null()) return e;
        if (auto e = opt_string_array("peers",    filter.peers);    !e.is_null()) return e;
        // Categories are an unordered_set<string>.
        const auto cit = pit->find("categories");
        if (cit != pit->end())
        {
            if (!cit->is_array())
                return make_error("invalid_params",
                    "query_metrics: params.categories must be an array of strings");
            for (const auto &v : *cit)
            {
                if (!v.is_string())
                    return make_error("invalid_params",
                        "query_metrics: params.categories entries must be strings");
                filter.categories.insert(v.get<std::string>());
            }
        }
    }
    json result = host.broker().query_metrics(filter);
    // HEP-CORE-0033 §12.2.2 — script-side augmentation: pass the
    // request's params (or empty object if absent) so the script can
    // see the same filter view AdminService applied, then mutate the
    // metrics dict (e.g. add custom aggregates computed in on_tick).
    if (auto *api = host.hub_api())
    {
        json params_for_hook = (pit != request.end() && pit->is_object())
                                   ? *pit
                                   : json::object();
        api->augment_query_metrics(params_for_hook, result);
    }
    return make_ok(std::move(result));
}

// ============================================================================
// Per-method handlers — Phase 6.2c (control block — HEP-0033 §11.2)
// ============================================================================
//
// Each delegates to an existing mutator on `host.broker()` or to a
// HubHost-level operation.  The broker mutators are queue-based —
// fire-and-forget — so the admin response is the accept ack, NOT a
// completion signal.
//
// TOCTOU note: pre-check existence via `host.state().channel(name)`
// then call the broker mutator.  If the channel deregisters between
// the two, the broker's idempotent-drop kicks in and we lose the
// `not_found` discrimination on the race.  Acceptable — operator
// sees a `queued: true` response that turns into a no-op.

json AdminService::Impl::handle_close_channel(const json &request)
{
    const auto pit = request.find("params");
    if (pit == request.end() || !pit->is_object() ||
        !pit->contains("channel") || !(*pit)["channel"].is_string())
        return make_error("invalid_params",
                          "close_channel requires params.channel (string)");
    const auto &name = (*pit)["channel"].get_ref<const std::string &>();
    if (!host.state().channel(name))
        return make_error("not_found",
                          std::string("channel '") + name + "' not registered");
    host.broker().request_close_channel(name);
    return make_ok(json{{"queued", true}, {"channel", name}});
}

json AdminService::Impl::handle_broadcast_channel(const json &request)
{
    const auto pit = request.find("params");
    if (pit == request.end() || !pit->is_object())
        return make_error("invalid_params",
                          "broadcast_channel requires params.{channel,message}");
    const auto cit = pit->find("channel");
    const auto mit = pit->find("message");
    if (cit == pit->end() || !cit->is_string() ||
        mit == pit->end() || !mit->is_string())
        return make_error("invalid_params",
                          "broadcast_channel requires params.channel (string) "
                          "and params.message (string)");
    const auto &channel = cit->get_ref<const std::string &>();
    const auto &message = mit->get_ref<const std::string &>();
    if (!host.state().channel(channel))
        return make_error("not_found",
                          std::string("channel '") + channel + "' not registered");
    // Optional `data` payload (string).
    std::string data;
    const auto dit = pit->find("data");
    if (dit != pit->end())
    {
        if (!dit->is_string())
            return make_error("invalid_params",
                              "broadcast_channel: params.data must be a string if present");
        data = dit->get<std::string>();
    }
    host.broker().request_broadcast_channel(channel, message, data);
    return make_ok(json{{"queued", true}, {"channel", channel}, {"message", message}});
}

json AdminService::Impl::handle_request_shutdown(const json & /*request*/)
{
    // Returns OK *before* the host actually stops — the admin socket
    // would otherwise be torn down with the response in flight.
    // Caller may follow up with `ping` and observe the EOF when the
    // socket closes.  See note L6 in the AdminService static review
    // for a future test that asserts that EOF behaviour.
    host.request_shutdown();
    return make_ok(json{{"shutdown_requested", true}});
}

} // namespace pylabhub::admin
