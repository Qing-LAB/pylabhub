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

#include "utils/config/hub_admin_config.hpp"
#include "utils/hub_host.hpp"
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
            reply = make_error("internal_error", e.what());
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

    // §11.2 method names not yet implemented (Phase 6.2b/c).  Listed
    // explicitly so a typo in the client surfaces as `unknown_method`
    // rather than blending into the catch-all.
    static const char *const kKnownStubs[] = {
        // Query (Phase 6.2b)
        "list_channels", "get_channel", "list_roles", "get_role",
        "list_bands", "list_peers", "query_metrics",
        // Control (Phase 6.2c)
        "close_channel", "broadcast_channel", "request_shutdown",
        // Deferred — see REVIEW_AdminService_2026-05-01.md §2.2
        "list_known_roles", "add_known_role", "remove_known_role",
        "revoke_role", "reload_config", "exec_python",
    };
    for (const char *m : kKnownStubs)
    {
        if (method == m)
            return make_error("not_implemented",
                              std::string("method '") + method +
                              "' not yet implemented (Phase 6.2a skeleton)");
    }

    return make_error("unknown_method",
                      std::string("unrecognised method '") + method + "'");
}

} // namespace pylabhub::admin
