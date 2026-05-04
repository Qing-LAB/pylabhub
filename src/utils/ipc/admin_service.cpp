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
#include "utils/hub_host.hpp"
#include "utils/hub_metrics_filter.hpp"
#include "utils/hub_state.hpp"
#include "utils/hub_state_json.hpp"
#include "utils/logger.hpp"
#include "utils/timeout_constants.hpp"

#include "cppzmq/zmq.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pylabhub::admin
{

using nlohmann::json;

namespace
{

/// Returns true if `endpoint` is a `tcp://` URL whose host portion is
/// the IPv4 loopback (`127.0.0.1`) or the literal `localhost`.  Used
/// to enforce the §11.3 invariant that token-skipping configurations
/// must bind only to loopback.
bool is_loopback_tcp_endpoint(std::string_view endpoint)
{
    constexpr std::string_view kPrefix = "tcp://";
    if (endpoint.substr(0, kPrefix.size()) != kPrefix)
        return false;  // non-tcp transports (inproc/ipc) handled separately
    auto rest = endpoint.substr(kPrefix.size());
    return rest.starts_with("127.0.0.1:") || rest.starts_with("localhost:");
}

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
    bool                           token_required;
    bool                           dev_mode;

    std::atomic<bool>              stop_requested{false};
    std::string                    bound_endpoint;

    Impl(zmq::context_t &c, hub_host::HubHost &h)
        : ctx(c), host(h)
    {}

    /// Top-level dispatcher.  Validates envelope shape, runs the
    /// token gate, then routes the method name to one of the
    /// `handle_*` member functions below.  All §11.2 method names
    /// (wired or deferred) are explicit; an unknown name surfaces
    /// as `unknown_method` rather than blending into a catch-all.
    json dispatch(const json &request);

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
    impl_->token_required = cfg.token_required;
    impl_->dev_mode       = cfg.dev_mode;

    // §11.3 invariant: when token gating is off, bind must be loopback.
    // Enforced at construction so misconfiguration is caught before
    // the socket is opened.  `dev_mode==true` is allowed to imply
    // `token_required==false` per HEP §11.3, so this check covers
    // both forms.
    if (!cfg.token_required && !is_loopback_tcp_endpoint(cfg.endpoint))
    {
        throw std::invalid_argument(
            std::string("AdminService: token_required=false requires loopback "
                        "endpoint (tcp://127.0.0.1:* or tcp://localhost:*); "
                        "got '") + cfg.endpoint + "'");
    }

    // §11.3 invariant: when token gating is on, the admin token must
    // be non-empty (otherwise every request silently authenticates).
    if (cfg.token_required && admin_token.empty())
    {
        throw std::invalid_argument(
            "AdminService: token_required=true but admin_token is empty "
            "(vault not unlocked? — HubConfig::load_keypair must run before "
            "AdminService construction)");
    }
}

AdminService::~AdminService() = default;

// ============================================================================
// Run / stop
// ============================================================================

void AdminService::run()
{
    zmq::socket_t sock(impl_->ctx, zmq::socket_type::rep);

    // Allow rebind in tests where the same endpoint may be reused
    // after a fast teardown.  Matches the pattern broker_service uses.
    sock.set(zmq::sockopt::linger, 0);

    sock.bind(impl_->endpoint);
    impl_->bound_endpoint = sock.get(zmq::sockopt::last_endpoint);

    LOGGER_INFO("[admin] AdminService listening on {} (token_required={}, dev_mode={})",
                impl_->bound_endpoint, impl_->token_required, impl_->dev_mode);

    while (!impl_->stop_requested.load(std::memory_order_acquire))
    {
        zmq::pollitem_t items[] = {{ static_cast<void*>(sock), 0, ZMQ_POLLIN, 0 }};
        const int rc = zmq::poll(items, 1, std::chrono::milliseconds(kAdminPollIntervalMs));
        if (rc <= 0)
            continue;

        zmq::message_t req_msg;
        if (!sock.recv(req_msg, zmq::recv_flags::dontwait))
            continue;

        json reply;
        try
        {
            const auto request = json::parse(
                std::string_view(static_cast<const char *>(req_msg.data()),
                                 req_msg.size()));
            reply = impl_->dispatch(request);
        }
        catch (const json::parse_error &e)
        {
            reply = make_error("invalid_request",
                               std::string("malformed JSON: ") + e.what());
        }
        catch (const std::exception &e)
        {
            // Last-resort safety net.  Specific dispatcher failure
            // modes should produce typed errors via dispatch() itself;
            // this catches programmer errors and unknown exceptions.
            // Code per HEP-0033 §11.5 catalog.
            reply = make_error("internal", e.what());
        }

        const std::string reply_str = reply.dump();
        sock.send(zmq::buffer(reply_str), zmq::send_flags::none);
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

json AdminService::Impl::dispatch(const json &request)
{
    if (!request.is_object())
        return make_error("invalid_request", "request must be a JSON object");

    if (!request.contains("method") || !request["method"].is_string())
        return make_error("invalid_request", "missing or non-string 'method'");

    const std::string method = request["method"].get<std::string>();

    // ── Token gate (§11.3) ──────────────────────────────────────────────────
    //
    // When token_required, every method requires a matching token.
    // Token comparison uses constant-time-equal-length form: the
    // admin_token from the vault is fixed-length (64 hex), so the
    // size check + char-by-char compare is sufficient.  No timing
    // sensitivity beyond that — the admin endpoint is loopback or
    // VPN'd in any realistic deployment.  See note L2 in the
    // AdminService static review (commit ledger): if the admin
    // endpoint ever goes public, switch to a known-constant-time
    // memcmp variant the compiler can't optimize away.
    if (token_required)
    {
        const auto token_it = request.find("token");
        if (token_it == request.end() || !token_it->is_string())
            return make_error("unauthorized", "missing 'token' field");
        const auto &token = token_it->get_ref<const std::string &>();
        if (token.size() != admin_token.size())
            return make_error("unauthorized", "invalid token");
        // size-equal compare (no early exit on first mismatch — tiny
        // win against side-channel; mostly habit at this string size)
        bool eq = true;
        for (size_t i = 0; i < token.size(); ++i)
            eq &= (token[i] == admin_token[i]);
        if (!eq)
            return make_error("unauthorized", "invalid token");
    }

    // ── Method dispatch table ───────────────────────────────────────────────
    //
    // Pointer-to-member pairs.  Adding a method = add a row + define
    // the handler.  No fall-through; first-match wins.  Static so we
    // construct it once per process.
    using HandlerPtr = json (Impl::*)(const json &);
    struct MethodEntry { std::string_view name; HandlerPtr handler; };
    static constexpr MethodEntry kMethods[] = {
        // Built-in (Phase 6.2a)
        {"ping",              &Impl::handle_ping},
        // Query (Phase 6.2b — HEP-0033 §11.2 query block)
        {"list_channels",     &Impl::handle_list_channels},
        {"get_channel",       &Impl::handle_get_channel},
        {"list_roles",        &Impl::handle_list_roles},
        {"get_role",          &Impl::handle_get_role},
        {"list_bands",        &Impl::handle_list_bands},
        {"list_peers",        &Impl::handle_list_peers},
        {"query_metrics",     &Impl::handle_query_metrics},
        // Control (Phase 6.2c — HEP-0033 §11.2 control block)
        {"close_channel",     &Impl::handle_close_channel},
        {"broadcast_channel", &Impl::handle_broadcast_channel},
        {"request_shutdown",  &Impl::handle_request_shutdown},
    };
    for (const auto &m : kMethods)
    {
        if (method == m.name)
            return (this->*m.handler)(request);
    }

    // ── Deferred methods (HEP-0035 / §16 #1 / §16 #9) ───────────────────────
    //
    // Listed explicitly so a typo lands on `unknown_method` while a
    // deferred-but-spec'd method lands on `not_implemented`.  Updates
    // to this list:
    //   - When HEP-0035 lands → remove the three known_roles entries.
    //   - When §16 #1 / §16 #9 close → remove revoke_role / reload_config.
    //
    // `exec_python` was removed entirely (was previously deferred for
    // Phase 7 wiring).  See HEP-CORE-0033 §17 "No remote code
    // injection" — the hub deliberately does not accept arbitrary
    // executable code over the wire.  Operator scripting access is
    // routed through the future Python SDK (composes structured
    // AdminService RPCs locally on the operator's host); see
    // `docs/todo/API_TODO.md` "pylabhub Python client SDK".
    static constexpr std::string_view kDeferredMethods[] = {
        "list_known_roles", "add_known_role", "remove_known_role", // HEP-0035
        "revoke_role",                                              // §16 #1
        "reload_config",                                            // §16 #9
    };
    for (const auto &m : kDeferredMethods)
    {
        if (method == m)
            return make_error("not_implemented",
                              std::string("method '") + method +
                              "' is deferred to a later HEP / phase");
    }

    return make_error("unknown_method",
                      std::string("unrecognised method '") + method + "'");
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
        channels[name] = pylabhub::hub::channel_to_json(ch);
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
    auto ch = host.state().channel(name);
    if (!ch)
        return make_error("not_found",
                          std::string("channel '") + name + "' not registered");
    return make_ok(pylabhub::hub::channel_to_json(*ch));
}

json AdminService::Impl::handle_list_roles(const json & /*request*/)
{
    const auto snap = host.state().snapshot();
    json roles = json::object();
    for (const auto &[uid, r] : snap.roles)
        roles[uid] = pylabhub::hub::role_to_json(r);
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
    return make_ok(host.broker().query_metrics(filter));
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
