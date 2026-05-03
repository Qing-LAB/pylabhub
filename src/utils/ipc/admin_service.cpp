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
    /// token gate, then routes the method name.  Phase 6.2a only
    /// resolves `ping`; everything else is `not_implemented`.
    json dispatch(const json &request);
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
// dispatch — Phase 6.2a routing only
// ============================================================================

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
    // VPN'd in any realistic deployment.
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

    // ── Method routing ──────────────────────────────────────────────────────

    if (method == "ping")
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

    // ── Phase 6.2b: Query methods (HEP-CORE-0033 §11.2 query block) ─────────
    //
    // All read-through `host.state()` (HubState) accessors.  No mutation;
    // the broker keeps its single-mutator invariant.  Per HEP §11.5 the
    // not_found code is the right one when a `get_*` target does not
    // exist; invalid_params when the client omitted the required field.

    if (method == "list_channels")
    {
        const auto snap = host.state().snapshot();
        json channels = json::object();
        for (const auto &[name, ch] : snap.channels)
            channels[name] = pylabhub::hub::channel_to_json(ch);
        return make_ok(std::move(channels));
    }

    if (method == "get_channel")
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

    if (method == "list_roles")
    {
        const auto snap = host.state().snapshot();
        json roles = json::object();
        for (const auto &[uid, r] : snap.roles)
            roles[uid] = pylabhub::hub::role_to_json(r);
        return make_ok(std::move(roles));
    }

    if (method == "get_role")
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

    if (method == "list_bands")
    {
        const auto snap = host.state().snapshot();
        json bands = json::object();
        for (const auto &[name, b] : snap.bands)
            bands[name] = pylabhub::hub::band_to_json(b);
        return make_ok(std::move(bands));
    }

    if (method == "list_peers")
    {
        const auto snap = host.state().snapshot();
        json peers = json::object();
        for (const auto &[uid, p] : snap.peers)
            peers[uid] = pylabhub::hub::peer_to_json(p);
        return make_ok(std::move(peers));
    }

    if (method == "query_metrics")
    {
        // Optional `params` filter — narrows the response to specific
        // categories / channels / roles / bands / peers.  Empty
        // params (or empty filter fields) means include everything.
        // Delegates to the broker's existing metrics aggregator
        // (HEP-0019 + HEP-0033 §9.4) so admin RPC and the legacy
        // query path emit identical JSON.
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

    // ── Phase 6.2c: Control methods (HEP-CORE-0033 §11.2 control block) ─────
    //
    // Each delegates to an existing mutator on `host.broker()` or to a
    // HubHost-level operation (`request_shutdown`).  The broker mutators
    // are queue-based — fire-and-forget — so the admin response is the
    // accept ack, NOT a completion signal.  Per §11.5 the codes are:
    //   - invalid_params: required field missing / wrong type
    //   - not_found: target channel doesn't exist (where pre-checkable)
    //   - ok: request accepted into the broker queue (or applied for
    //         host-level ops like request_shutdown)

    if (method == "close_channel")
    {
        const auto pit = request.find("params");
        if (pit == request.end() || !pit->is_object() ||
            !pit->contains("channel") || !(*pit)["channel"].is_string())
            return make_error("invalid_params",
                              "close_channel requires params.channel (string)");
        const auto &name = (*pit)["channel"].get_ref<const std::string &>();
        // Pre-check existence so a typo produces not_found rather than
        // a silent no-op (BrokerService::request_close_channel is
        // idempotent and silently drops unknown names).
        if (!host.state().channel(name))
            return make_error("not_found",
                              std::string("channel '") + name + "' not registered");
        host.broker().request_close_channel(name);
        return make_ok(json{{"queued", true}, {"channel", name}});
    }

    if (method == "broadcast_channel")
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

    if (method == "request_shutdown")
    {
        // Returns OK *before* the host actually stops — the admin
        // socket would otherwise be torn down with the response in
        // flight.  Caller may follow up with `query_metrics` / `ping`
        // and observe the EOF when the socket closes.
        host.request_shutdown();
        return make_ok(json{{"shutdown_requested", true}});
    }

    // ── Deferred methods (HEP-0035 / §16 #1 / §16 #9 / Phase 7) ─────────────
    static const char *const kDeferredMethods[] = {
        "list_known_roles", "add_known_role", "remove_known_role", // HEP-0035
        "revoke_role",                                              // §16 #1
        "reload_config",                                            // §16 #9
        "exec_python",                                              // Phase 7
    };
    for (const char *m : kDeferredMethods)
    {
        if (method == m)
            return make_error("not_implemented",
                              std::string("method '") + method +
                              "' is deferred to a later HEP / phase");
    }

    return make_error("unknown_method",
                      std::string("unrecognised method '") + method + "'");
}

} // namespace pylabhub::admin
