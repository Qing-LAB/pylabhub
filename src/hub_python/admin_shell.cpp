/**
 * @file admin_shell.cpp
 * @brief AdminShell lifecycle module implementation.
 */
#include "admin_shell.hpp"
#include "python_interpreter.hpp"
#include "plh_datahub.hpp"

#include "utils/zmq_context.hpp"

#include "cppzmq/zmq.hpp"
#include "utils/json_fwd.hpp"
#include <sodium.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace pylabhub
{

// ---------------------------------------------------------------------------
// AdminShell::Impl — all private state
// ---------------------------------------------------------------------------

struct AdminShell::Impl
{
    zmq::socket_t         socket;
    std::thread           worker;
    std::atomic<bool>     running{false};
    std::string           token; // pre-shared auth token (empty = no auth)

    explicit Impl()
        : socket(pylabhub::hub::get_zmq_context(), zmq::socket_type::rep)
    {
        socket.set(zmq::sockopt::linger, 0); // policy: always LINGER=0; see §ZMQ socket policy
    }

    // -----------------------------------------------------------------------
    // Startup / shutdown
    // -----------------------------------------------------------------------

    std::string bound_endpoint; // resolved after bind (e.g. port 0 → real port)

    void startup(const std::string& endpoint, const std::string& auth_token)
    {
        token = auth_token;
        socket.bind(endpoint);
        bound_endpoint = socket.get(zmq::sockopt::last_endpoint);
        LOGGER_INFO("AdminShell: listening on {}", bound_endpoint);
        if (token.empty())
        {
            LOGGER_INFO("AdminShell: no token configured — any local connection is accepted");
        }
        else
        {
            LOGGER_INFO("AdminShell: token authentication enabled");
        }

        // `token` is written above (lines 45+) before this thread ctor; the
        // std::thread constructor provides a happens-before barrier, so the
        // worker thread is guaranteed to observe the token value written here.
        running.store(true, std::memory_order_release);
        worker = std::thread([this] { run(); });
    }

    void shutdown()
    {
        running.store(false, std::memory_order_release);
        // Close socket so zmq_poll (or blocking recv) unblocks.
        try
        {
            socket.close();
        }
        catch (const zmq::error_t& e)
        {
            LOGGER_WARN("AdminShell: socket close error on shutdown: {}", e.what());
        }
        if (worker.joinable())
        {
            worker.join();
        }
        LOGGER_INFO("AdminShell: shutdown complete");
    }

    // -----------------------------------------------------------------------
    // Worker thread — REP receive/reply loop
    // -----------------------------------------------------------------------

    void run()
    {
        while (running.load(std::memory_order_acquire))
        {
            try
            {
                // Short-timeout poll so we can check the stop flag.
                zmq::pollitem_t items[] = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
                zmq::poll(items, 1,
                          std::chrono::milliseconds(pylabhub::kAdminPollIntervalMs));

                if (!running.load(std::memory_order_acquire))
                    break;

                if ((items[0].revents & ZMQ_POLLIN) == 0)
                    continue;

                zmq::message_t msg;
                auto res = socket.recv(msg, zmq::recv_flags::dontwait);
                if (!res)
                    continue;

                // Reject oversized payloads before string construction and JSON parse
                // (Pitfall 14: auth must not fire AFTER OOM — check size first).
                static constexpr size_t kMaxRequestBytes = 1u << 20; // 1 MB
                if (msg.size() > kMaxRequestBytes)
                {
                    LOGGER_WARN("AdminShell: oversized request ({} bytes) — rejected",
                                msg.size());
                    const std::string rej =
                        nlohmann::json{{"success", false},
                                       {"output",  ""},
                                       {"error",   "request too large"}}.dump();
                    socket.send(zmq::buffer(rej), zmq::send_flags::none);
                    continue;
                }

                std::string raw(static_cast<const char*>(msg.data()), msg.size());
                std::string reply = handle_request(raw);

                // REP: must always send a reply before the next recv.
                socket.send(zmq::buffer(reply), zmq::send_flags::none);
            }
            catch (const zmq::error_t& e)
            {
                // ETERM (or ENOTSUP after close) — normal shutdown path.
                if (e.num() == ETERM || e.num() == ENOTSUP)
                    break;
                LOGGER_WARN("AdminShell: ZMQ error in worker: {}", e.what());
            }
            catch (const std::exception& e)
            {
                LOGGER_ERROR("AdminShell: unexpected error in worker: {}", e.what());
            }
        }
    }

    // -----------------------------------------------------------------------
    // Request handler — returns JSON string
    // -----------------------------------------------------------------------

    std::string handle_request(const std::string& raw)
    {
        auto error_reply = [](const std::string& msg) -> std::string
        {
            return nlohmann::json{{"success", false}, {"output", ""}, {"error", msg}}.dump();
        };

        nlohmann::json req;
        try
        {
            req = nlohmann::json::parse(raw);
        }
        catch (const nlohmann::json::parse_error&)
        {
            return error_reply("invalid JSON request");
        }

        // Token authentication (constant-time comparison via libsodium to
        // prevent timing side-channel attacks on the token value).
        if (!token.empty())
        {
            const std::string user_token = req.value("token", "");
            if (user_token.size() != token.size() ||
                sodium_memcmp(user_token.data(), token.data(), token.size()) != 0)
            {
                LOGGER_WARN("AdminShell: rejected request — invalid token");
                return error_reply("unauthorized");
            }
        }

        // Extract code.
        if (!req.contains("code") || !req["code"].is_string())
        {
            return error_reply("missing or invalid 'code' field");
        }
        const std::string code = req["code"].get<std::string>();

        // Execute via PythonInterpreter.
        PyExecResult result;
        try
        {
            result = PythonInterpreter::get_instance().exec(code);
        }
        catch (const std::exception& e)
        {
            return error_reply(std::string("interpreter error: ") + e.what());
        }

        return nlohmann::json{
            {"success", result.success},
            {"output",  result.output},
            {"error",   result.error}
        }.dump();
    }
};

// ---------------------------------------------------------------------------
// AdminShell — public interface
// ---------------------------------------------------------------------------

AdminShell::AdminShell() : pImpl(std::make_unique<Impl>()) {}
AdminShell::~AdminShell() = default;

// static
AdminShell& AdminShell::get_instance()
{
    static AdminShell instance;
    return instance;
}

void AdminShell::startup_()
{
    const auto& cfg    = HubConfig::get_instance();
    const auto  endpoint = cfg.admin_endpoint();

    // Optional pre-shared token from hub.json ["admin"]["token"].
    // If absent, token is empty (any local connection is accepted).
    const std::string auth_token = cfg.admin_token();

    pImpl->startup(endpoint, auth_token);
}

void AdminShell::shutdown_()
{
    pImpl->shutdown();
}

std::string AdminShell::actual_endpoint() const
{
    return pImpl->bound_endpoint;
}

// ---------------------------------------------------------------------------
// Lifecycle module factory
// ---------------------------------------------------------------------------

namespace
{
void do_admin_shell_startup(const char* /*arg*/, void* /*userdata*/)
{
    AdminShell::get_instance().startup_();
}

void do_admin_shell_shutdown(const char* /*arg*/, void* /*userdata*/)
{
    AdminShell::get_instance().shutdown_();
}
} // namespace

// static
utils::ModuleDef AdminShell::GetLifecycleModule()
{
    utils::ModuleDef module("pylabhub::AdminShell");
    module.add_dependency("pylabhub::utils::Logger");
    module.add_dependency("pylabhub::HubConfig");
    module.add_dependency("pylabhub::PythonInterpreter");
    module.add_dependency("ZMQContext");
    module.set_startup(&do_admin_shell_startup);
    module.set_shutdown(&do_admin_shell_shutdown,
                        std::chrono::milliseconds(pylabhub::kMidTimeoutMs));
    return module;
}

} // namespace pylabhub
