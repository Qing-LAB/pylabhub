/**
 * @file admin_shell.cpp
 * @brief AdminShell lifecycle module implementation.
 */
#include "admin_shell.hpp"
#include "python_interpreter.hpp"
#include "plh_datahub.hpp"

#include "utils/zmq_context.hpp"

#include "cppzmq/zmq.hpp"
#include <nlohmann/json.hpp>

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
    }

    // -----------------------------------------------------------------------
    // Startup / shutdown
    // -----------------------------------------------------------------------

    void startup(const std::string& endpoint, const std::string& auth_token)
    {
        token = auth_token;
        socket.bind(endpoint);
        const std::string bound = socket.get(zmq::sockopt::last_endpoint);
        LOGGER_INFO("AdminShell: listening on {}", bound);
        if (token.empty())
        {
            LOGGER_INFO("AdminShell: no token configured — any local connection is accepted");
        }
        else
        {
            LOGGER_INFO("AdminShell: token authentication enabled");
        }

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
                zmq::poll(items, 1, std::chrono::milliseconds(100));

                if (!running.load(std::memory_order_acquire))
                    break;

                if ((items[0].revents & ZMQ_POLLIN) == 0)
                    continue;

                zmq::message_t msg;
                auto res = socket.recv(msg, zmq::recv_flags::dontwait);
                if (!res)
                    continue;

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

        // Token authentication.
        if (!token.empty())
        {
            if (req.value("token", "") != token)
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

    // Optional pre-shared token from hub.user.json ["admin"]["token"].
    // If absent, token is empty (any local connection is accepted).
    const std::string auth_token = cfg.admin_token();

    pImpl->startup(endpoint, auth_token);
}

void AdminShell::shutdown_()
{
    pImpl->shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle module factory
// ---------------------------------------------------------------------------

namespace
{
void do_admin_shell_startup(const char* /*arg*/)
{
    AdminShell::get_instance().startup_();
}

void do_admin_shell_shutdown(const char* /*arg*/)
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
    module.set_shutdown(&do_admin_shell_shutdown, std::chrono::seconds(5));
    return module;
}

} // namespace pylabhub
