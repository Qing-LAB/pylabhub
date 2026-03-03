/**
 * @file test_admin_shell.cpp
 * @brief Integration tests for HubShell admin shell — HP-C1 (reset deadlock)
 *        and HP-C2 (stdout/stderr leak on exec exception).
 *
 * Spawns `pylabhub-hubshell --dev <tmp_dir>` as a live subprocess, connects
 * to the admin ZMQ REP endpoint, sends Python code, and verifies responses.
 *
 * Pattern 3 (IsolatedProcessTest) — subprocess-per-test.
 */

#include "test_patterns.h"
#include "test_process_utils.h"
#include "test_entrypoint.h"

#include <cppzmq/zmq.hpp>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace pylabhub::tests::helper;
using json = nlohmann::json;

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::atomic<int> s_port_counter{0};

/// Returns the path to the staged pylabhub-hubshell binary.
static std::string hubshell_binary()
{
    return (fs::path(g_self_exe_path).parent_path() / ".." / "bin" / "pylabhub-hubshell")
               .string();
}

/// Allocate a pair of unique ports for broker + admin endpoints.
struct PortPair
{
    int broker;
    int admin;
};

static PortPair allocate_ports()
{
    // Base port chosen to avoid common conflicts. Each test gets a unique pair.
    constexpr int kBasePort = 15570;
    const int slot = s_port_counter.fetch_add(1);
    return {kBasePort + slot * 2, kBasePort + slot * 2 + 1};
}

static fs::path unique_temp_dir(const std::string& prefix)
{
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1);
    fs::path dir = fs::temp_directory_path()
                   / ("plh_integ_" + prefix + "_" + std::to_string(id) + "_"
                      + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir;
}

/// Write a minimal hub.json with custom ports.
static void write_hub_json(const fs::path& dir, const PortPair& ports)
{
    const json hub_json = {
        {"hub", {
            {"name",            "test-hub"},
            {"uid",             "HUB-TEST-00000001"},
            {"broker_endpoint", fmt::format("tcp://127.0.0.1:{}", ports.broker)},
            {"admin_endpoint",  fmt::format("tcp://127.0.0.1:{}", ports.admin)}
        }},
        {"broker", {
            {"channel_timeout_s",         10},
            {"consumer_liveness_check_s",  5}
        }},
        {"script", {
            {"type",                   "python"},
            {"path",                   "./script"},
            {"tick_interval_ms",       60000},
            {"health_log_interval_ms", 600000}
        }}
    };
    fs::create_directories(dir / "script" / "python");

    // Minimal hub script — no rich dependency
    {
        std::ofstream f(dir / "script" / "python" / "__init__.py");
        f << "def on_start(api): pass\n"
           << "def on_tick(api, tick): pass\n"
           << "def on_stop(api): pass\n";
    }

    {
        std::ofstream f(dir / "hub.json");
        f << hub_json.dump(2) << '\n';
    }
}

/// Send a JSON request to the admin shell and return the parsed response.
/// Returns nullopt if the receive times out.
static std::optional<json> admin_request(zmq::socket_t& sock, const std::string& code,
                                         int timeout_ms = 10000)
{
    json req{{"code", code}};
    std::string req_str = req.dump();
    sock.send(zmq::buffer(req_str), zmq::send_flags::none);

    zmq::message_t msg;
    sock.set(zmq::sockopt::rcvtimeo, timeout_ms);
    auto res = sock.recv(msg, zmq::recv_flags::none);
    if (!res)
        return std::nullopt;

    return json::parse(std::string(static_cast<const char*>(msg.data()), msg.size()));
}

/// Poll the admin endpoint until it responds, or timeout.
static bool wait_for_admin_ready(const std::string& endpoint, int timeout_ms = 15000)
{
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline)
    {
        try
        {
            zmq::context_t ctx(1);
            zmq::socket_t sock(ctx, zmq::socket_type::req);
            sock.set(zmq::sockopt::rcvtimeo, 1000);
            sock.set(zmq::sockopt::linger, 0);
            sock.connect(endpoint);

            json req{{"code", "pass"}};
            sock.send(zmq::buffer(req.dump()), zmq::send_flags::none);

            zmq::message_t msg;
            auto res = sock.recv(msg, zmq::recv_flags::none);
            if (res)
            {
                auto resp = json::parse(
                    std::string(static_cast<const char*>(msg.data()), msg.size()));
                if (resp.contains("success"))
                    return true;
            }
        }
        catch (...)
        {
            // Connection refused or other transient error — retry
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class AdminShellTest : public pylabhub::tests::IsolatedProcessTest {};

// ── HP-C1: pylabhub.reset() deadlock regression ─────────────────────────────

/// Calling pylabhub.reset() from within exec() must not deadlock.
/// The fix in pylabhub_module.cpp calls reset_namespace_unlocked() with GIL released.
TEST_F(AdminShellTest, HP_C1_Reset_NoDeadlock)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("hpc1_nodeadlock");
    write_hub_json(tmp, ports);

    // Spawn hubshell --dev <dir>
    WorkerProcess hubshell(hubshell_binary(), tmp.string(), {"--dev"});
    ASSERT_TRUE(hubshell.valid()) << "Failed to spawn hubshell";

    const std::string admin_ep = fmt::format("tcp://127.0.0.1:{}", ports.admin);
    ASSERT_TRUE(wait_for_admin_ready(admin_ep))
        << "Admin shell did not become ready within timeout";

    // Connect a persistent ZMQ REQ socket for the test sequence.
    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::req);
    sock.set(zmq::sockopt::linger, 0);
    sock.connect(admin_ep);

    // Set a variable, then reset — must complete within timeout (no deadlock).
    {
        auto resp = admin_request(sock, "x = 42");
        ASSERT_TRUE(resp.has_value()) << "Timed out setting variable";
        EXPECT_TRUE(resp->at("success").get<bool>());
    }
    {
        auto resp = admin_request(sock, "pylabhub.reset()");
        ASSERT_TRUE(resp.has_value()) << "pylabhub.reset() timed out — deadlock?";
        EXPECT_TRUE(resp->at("success").get<bool>())
            << "reset() failed: " << resp->value("error", "");
    }

    // Shut down gracefully.
    (void)admin_request(sock, "pylabhub.shutdown()", 5000);
    sock.close();
    ctx.close();
    int rc = hubshell.wait_for_exit();
    // Exit code 0 expected for graceful shutdown.
    EXPECT_EQ(rc, 0) << "hubshell exit code: " << rc
                      << "\nstderr:\n" << hubshell.get_stderr();
    fs::remove_all(tmp);
}

/// After pylabhub.reset(), previously defined variables must be gone.
TEST_F(AdminShellTest, HP_C1_Reset_ClearsNamespace)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("hpc1_clear");
    write_hub_json(tmp, ports);

    WorkerProcess hubshell(hubshell_binary(), tmp.string(), {"--dev"});
    ASSERT_TRUE(hubshell.valid());

    const std::string admin_ep = fmt::format("tcp://127.0.0.1:{}", ports.admin);
    ASSERT_TRUE(wait_for_admin_ready(admin_ep));

    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::req);
    sock.set(zmq::sockopt::linger, 0);
    sock.connect(admin_ep);

    // Define variable.
    {
        auto resp = admin_request(sock, "x = 42");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
    }

    // Verify it exists.
    {
        auto resp = admin_request(sock, "print(x)");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
        EXPECT_NE(resp->at("output").get<std::string>().find("42"), std::string::npos);
    }

    // Reset namespace.
    {
        auto resp = admin_request(sock, "pylabhub.reset()");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
    }

    // Variable should be gone — NameError.
    {
        auto resp = admin_request(sock, "print(x)");
        ASSERT_TRUE(resp.has_value());
        EXPECT_FALSE(resp->at("success").get<bool>())
            << "Expected NameError but exec succeeded";
        const auto error = resp->at("error").get<std::string>();
        EXPECT_NE(error.find("NameError"), std::string::npos)
            << "Expected NameError, got: " << error;
    }

    (void)admin_request(sock, "pylabhub.shutdown()", 5000);
    sock.close();
    ctx.close();
    EXPECT_EQ(hubshell.wait_for_exit(), 0);
    fs::remove_all(tmp);
}

// ── HP-C2: stdout/stderr leak on exec() exception ──────────────────────────

/// An exception in exec() must not leak stdout capture — subsequent exec()
/// calls must still capture output correctly.
TEST_F(AdminShellTest, HP_C2_Exception_StdoutRestored)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("hpc2_stdout");
    write_hub_json(tmp, ports);

    WorkerProcess hubshell(hubshell_binary(), tmp.string(), {"--dev"});
    ASSERT_TRUE(hubshell.valid());

    const std::string admin_ep = fmt::format("tcp://127.0.0.1:{}", ports.admin);
    ASSERT_TRUE(wait_for_admin_ready(admin_ep));

    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::req);
    sock.set(zmq::sockopt::linger, 0);
    sock.connect(admin_ep);

    // Trigger an exception.
    {
        auto resp = admin_request(sock, "1/0");
        ASSERT_TRUE(resp.has_value());
        EXPECT_FALSE(resp->at("success").get<bool>());
    }

    // Now send normal code — stdout must be captured, not leaked.
    {
        auto resp = admin_request(sock, "print('CAPTURED_OK')");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>())
            << "Normal exec failed after exception: " << resp->value("error", "");
        const auto output = resp->at("output").get<std::string>();
        EXPECT_NE(output.find("CAPTURED_OK"), std::string::npos)
            << "stdout not captured after exception. Output: " << output;
    }

    (void)admin_request(sock, "pylabhub.shutdown()", 5000);
    sock.close();
    ctx.close();
    EXPECT_EQ(hubshell.wait_for_exit(), 0);
    fs::remove_all(tmp);
}

/// Exception info must be returned in the response error field.
TEST_F(AdminShellTest, HP_C2_Exception_ErrorReturned)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("hpc2_error");
    write_hub_json(tmp, ports);

    WorkerProcess hubshell(hubshell_binary(), tmp.string(), {"--dev"});
    ASSERT_TRUE(hubshell.valid());

    const std::string admin_ep = fmt::format("tcp://127.0.0.1:{}", ports.admin);
    ASSERT_TRUE(wait_for_admin_ready(admin_ep));

    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::req);
    sock.set(zmq::sockopt::linger, 0);
    sock.connect(admin_ep);

    // Division by zero — must get ZeroDivisionError in the error field.
    {
        auto resp = admin_request(sock, "1/0");
        ASSERT_TRUE(resp.has_value());
        EXPECT_FALSE(resp->at("success").get<bool>());
        const auto error = resp->at("error").get<std::string>();
        EXPECT_NE(error.find("ZeroDivisionError"), std::string::npos)
            << "Expected ZeroDivisionError, got: " << error;
    }

    (void)admin_request(sock, "pylabhub.shutdown()", 5000);
    sock.close();
    ctx.close();
    EXPECT_EQ(hubshell.wait_for_exit(), 0);
    fs::remove_all(tmp);
}
