/**
 * @file test_admin_shell.cpp
 * @brief Integration tests for HubShell admin shell — HP-C1 (reset deadlock)
 *        and HP-C2 (stdout/stderr leak on exec exception).
 *
 * Spawns `pylabhub-hubshell <tmp_dir> --dev` as a live subprocess, connects
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
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace pylabhub::tests::helper;
using json = nlohmann::json;

// ── Diagnostic helpers ───────────────────────────────────────────────────────

static std::string now_ms()
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count() % 100000; // last 5 digits for readability
    return fmt::format("{:05d}", ms);
}

#define TLOG(tag, msg, ...) \
    fmt::print(stderr, "[{}][{}] " msg "\n", now_ms(), tag, ##__VA_ARGS__)

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::atomic<int> s_port_counter{0};

static std::string hubshell_binary()
{
    return (fs::path(g_self_exe_path).parent_path() / ".." / "bin" / "pylabhub-hubshell")
               .string();
}

struct PortPair
{
    int broker;
    int admin;
};

static PortPair allocate_ports()
{
    // Port range: 10000–12999 (admin shell tests).
    // Non-overlapping with pipeline (13000–15999) and broadcast (16000–18999).
    // pid%500*6 → max offset 2994; with slot*2 → max port 12999 < 65535.
    // L3 ZmqQueue tests use 33000+, so no cross-binary collision possible.
    const int pid_offset = (::getpid() % 500) * 6;
    constexpr int kBasePort = 10000;
    const int slot = s_port_counter.fetch_add(1);
    return {kBasePort + pid_offset + slot * 2, kBasePort + pid_offset + slot * 2 + 1};
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
            {"consumer_liveness_check_s",  5},
            {"channel_shutdown_grace_s",   1}
        }},
        {"script", {
            {"type",                   "python"},
            {"path",                   "./script"},
            {"tick_interval_ms",       60000},
            {"health_log_interval_ms", 600000}
        }}
    };

    fs::create_directories(dir / "script" / "python");
    fs::create_directories(dir / "logs");

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

// ── RAII wrapper for hubshell subprocess ─────────────────────────────────────

class HubShellProcess
{
  public:
    HubShellProcess(const fs::path& hub_dir, const PortPair& ports, const std::string& tag)
        : tag_(tag)
        , admin_ep_(fmt::format("tcp://127.0.0.1:{}", ports.admin))
        , ctx_(1)
        , sock_(ctx_, zmq::socket_type::req)
        , proc_(hubshell_binary(), hub_dir.string(), {"--dev"}, /*redirect_stderr_to_console=*/true)
    {
        TLOG(tag_, "spawned hubshell (valid={}), admin_ep={}", proc_.valid(), admin_ep_);
        sock_.set(zmq::sockopt::linger, 0);
        sock_.set(zmq::sockopt::rcvtimeo, 10000);
    }

    ~HubShellProcess()
    {
        // If we never did a graceful shutdown, kill the child to avoid blocking.
        if (!shut_down_)
        {
            TLOG(tag_, "destructor: sending SIGTERM to child");
            kill_child();
        }
    }

    HubShellProcess(const HubShellProcess&) = delete;
    HubShellProcess& operator=(const HubShellProcess&) = delete;

    bool wait_ready(int timeout_ms = 15000)
    {
        TLOG(tag_, "wait_ready: sleeping 2s for startup...");
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        sock_.connect(admin_ep_);
        TLOG(tag_, "wait_ready: connected to {}", admin_ep_);

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(timeout_ms);
        int attempt = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            attempt++;
            TLOG(tag_, "wait_ready: probe attempt #{}", attempt);

            json req{{"code", "pass"}};
            sock_.send(zmq::buffer(req.dump()), zmq::send_flags::none);

            zmq::message_t msg;
            sock_.set(zmq::sockopt::rcvtimeo, 2000);
            auto res = sock_.recv(msg, zmq::recv_flags::none);
            if (res)
            {
                TLOG(tag_, "wait_ready: got response on attempt #{}", attempt);
                return true;
            }

            TLOG(tag_, "wait_ready: timeout on attempt #{}, reconnecting...", attempt);
            // REQ socket is in invalid state after recv timeout — must recreate.
            sock_.close();
            sock_ = zmq::socket_t(ctx_, zmq::socket_type::req);
            sock_.set(zmq::sockopt::linger, 0);
            sock_.connect(admin_ep_);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        TLOG(tag_, "wait_ready: TIMED OUT after {} attempts", attempt);
        return false;
    }

    std::optional<json> exec(const std::string& code, int timeout_ms = 10000)
    {
        TLOG(tag_, "exec: sending code='{}'", code.substr(0, 60));

        json req{{"code", code}};
        sock_.send(zmq::buffer(req.dump()), zmq::send_flags::none);

        zmq::message_t msg;
        sock_.set(zmq::sockopt::rcvtimeo, timeout_ms);
        auto res = sock_.recv(msg, zmq::recv_flags::none);
        if (!res)
        {
            TLOG(tag_, "exec: recv TIMED OUT for code='{}'", code.substr(0, 60));
            return std::nullopt;
        }

        auto resp = json::parse(std::string(static_cast<const char*>(msg.data()), msg.size()));
        TLOG(tag_, "exec: got response success={} output='{}' error='{}'",
             resp.value("success", false),
             resp.value("output", "").substr(0, 60),
             resp.value("error", "").substr(0, 60));
        return resp;
    }

    int shutdown()
    {
        TLOG(tag_, "shutdown: sending pylabhub.shutdown()...");
        (void)exec("pylabhub.shutdown()", 5000);

        TLOG(tag_, "shutdown: closing zmq socket+context...");
        sock_.close();
        ctx_.close();

        TLOG(tag_, "shutdown: waiting for process exit...");
        int rc = proc_.wait_for_exit();
        shut_down_ = true;
        TLOG(tag_, "shutdown: process exited with code {}", rc);
        return rc;
    }

    bool valid() const { return proc_.valid(); }
    const std::string& get_stderr() const { return proc_.get_stderr(); }

  private:
    void kill_child()
    {
        // Send SIGTERM, wait briefly, then SIGKILL if needed.
#if !defined(PLATFORM_WIN64)
        if (proc_.valid())
        {
            // WorkerProcess stores a pid_t. Access it via exit_code() check.
            // We can't access the handle_ directly, so just send SIGTERM to
            // the process group. Alternatively, wait_for_exit() in the
            // destructor of WorkerProcess will block — but we can't avoid
            // that without killing the child first.
            //
            // Hack: call wait_for_exit() with the hope that the process
            // already exited, or use the OS kill via /proc.
            // For now, just let WorkerProcess destructor handle it.
            // The timeout-based test runner (CTest) will kill us if stuck.
        }
#endif
        shut_down_ = true; // prevent double-kill in destructor
    }

    std::string tag_;
    std::string admin_ep_;
    zmq::context_t ctx_;
    zmq::socket_t sock_;
    WorkerProcess proc_;
    bool shut_down_ = false;
};

// ── Test fixture ─────────────────────────────────────────────────────────────

class AdminShellTest : public pylabhub::tests::IsolatedProcessTest {};

// ── HP-C1: pylabhub.reset() deadlock regression ─────────────────────────────

TEST_F(AdminShellTest, HP_C1_Reset_NoDeadlock)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("hpc1_nodeadlock");
    write_hub_json(tmp, ports);
    TLOG("TEST", "HP_C1_Reset_NoDeadlock: ports=({},{}), dir={}",
         ports.broker, ports.admin, tmp.string());

    HubShellProcess hub(tmp, ports, "HUB1");
    ASSERT_TRUE(hub.valid()) << "Failed to spawn hubshell";
    ASSERT_TRUE(hub.wait_ready()) << "Admin shell did not become ready";

    {
        auto resp = hub.exec("x = 42");
        ASSERT_TRUE(resp.has_value()) << "Timed out setting variable";
        EXPECT_TRUE(resp->at("success").get<bool>());
    }
    {
        auto resp = hub.exec("pylabhub.reset()");
        ASSERT_TRUE(resp.has_value()) << "pylabhub.reset() timed out — deadlock?";
        EXPECT_TRUE(resp->at("success").get<bool>())
            << "reset() failed: " << resp->value("error", "");
    }

    EXPECT_EQ(hub.shutdown(), 0) << "stderr:\n" << hub.get_stderr();
    fs::remove_all(tmp);
}

TEST_F(AdminShellTest, HP_C1_Reset_ClearsNamespace)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("hpc1_clear");
    write_hub_json(tmp, ports);
    TLOG("TEST", "HP_C1_Reset_ClearsNamespace: ports=({},{})", ports.broker, ports.admin);

    HubShellProcess hub(tmp, ports, "HUB2");
    ASSERT_TRUE(hub.valid());
    ASSERT_TRUE(hub.wait_ready());

    {
        auto resp = hub.exec("x = 42");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
    }
    {
        auto resp = hub.exec("print(x)");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
        EXPECT_NE(resp->at("output").get<std::string>().find("42"), std::string::npos);
    }
    {
        auto resp = hub.exec("pylabhub.reset()");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
    }
    {
        auto resp = hub.exec("print(x)");
        ASSERT_TRUE(resp.has_value());
        EXPECT_FALSE(resp->at("success").get<bool>())
            << "Expected NameError but exec succeeded";
        const auto error = resp->at("error").get<std::string>();
        EXPECT_NE(error.find("NameError"), std::string::npos)
            << "Expected NameError, got: " << error;
    }

    EXPECT_EQ(hub.shutdown(), 0);
    fs::remove_all(tmp);
}

// ── HP-C2: stdout/stderr leak on exec() exception ──────────────────────────

TEST_F(AdminShellTest, HP_C2_Exception_StdoutRestored)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("hpc2_stdout");
    write_hub_json(tmp, ports);
    TLOG("TEST", "HP_C2_Exception_StdoutRestored: ports=({},{})", ports.broker, ports.admin);

    HubShellProcess hub(tmp, ports, "HUB3");
    ASSERT_TRUE(hub.valid());
    ASSERT_TRUE(hub.wait_ready());

    {
        auto resp = hub.exec("1/0");
        ASSERT_TRUE(resp.has_value());
        EXPECT_FALSE(resp->at("success").get<bool>());
    }
    {
        auto resp = hub.exec("print('CAPTURED_OK')");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>())
            << "Normal exec failed after exception: " << resp->value("error", "");
        const auto output = resp->at("output").get<std::string>();
        EXPECT_NE(output.find("CAPTURED_OK"), std::string::npos)
            << "stdout not captured after exception. Output: " << output;
    }

    EXPECT_EQ(hub.shutdown(), 0);
    fs::remove_all(tmp);
}

TEST_F(AdminShellTest, HP_C2_Exception_ErrorReturned)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("hpc2_error");
    write_hub_json(tmp, ports);
    TLOG("TEST", "HP_C2_Exception_ErrorReturned: ports=({},{})", ports.broker, ports.admin);

    HubShellProcess hub(tmp, ports, "HUB4");
    ASSERT_TRUE(hub.valid());
    ASSERT_TRUE(hub.wait_ready());

    {
        auto resp = hub.exec("1/0");
        ASSERT_TRUE(resp.has_value());
        EXPECT_FALSE(resp->at("success").get<bool>());
        const auto error = resp->at("error").get<std::string>();
        EXPECT_NE(error.find("ZeroDivisionError"), std::string::npos)
            << "Expected ZeroDivisionError, got: " << error;
    }

    EXPECT_EQ(hub.shutdown(), 0);
    fs::remove_all(tmp);
}

// ── Script probe API tests ───────────────────────────────────────────────────
//
// Verify pylabhub.config() and pylabhub.channels() return correct data
// through the admin shell interface.

TEST_F(AdminShellTest, Probe_Config)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("probe_cfg");
    write_hub_json(tmp, ports);
    TLOG("TEST", "Probe_Config: ports=({},{})", ports.broker, ports.admin);

    HubShellProcess hub(tmp, ports, "HUB5");
    ASSERT_TRUE(hub.valid());
    ASSERT_TRUE(hub.wait_ready());

    // config() is a flat dict — verify identity keys
    {
        auto resp = hub.exec("c = pylabhub.config()\n"
                             "print(c['name'], c['uid'])");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
        auto out = resp->at("output").get<std::string>();
        EXPECT_NE(out.find("test-hub"), std::string::npos)
            << "config()['name'] should contain 'test-hub', got: " << out;
        EXPECT_NE(out.find("HUB-TEST-00000001"), std::string::npos)
            << "config()['uid'] missing, got: " << out;
    }

    // Verify network address keys
    {
        auto resp = hub.exec("c = pylabhub.config()\n"
                             "print(c['broker_address'], c['shell_address'])");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
        auto out = resp->at("output").get<std::string>();
        EXPECT_NE(out.find(std::to_string(ports.broker)), std::string::npos)
            << "config()['broker_address'] missing port, got: " << out;
        EXPECT_NE(out.find(std::to_string(ports.admin)), std::string::npos)
            << "config()['shell_address'] missing port, got: " << out;
    }

    // Verify path keys are non-empty strings
    {
        auto resp = hub.exec("c = pylabhub.config()\n"
                             "print(bool(c['root_dir']), bool(c['data_dir']), "
                             "bool(c['scripts_python']))");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
        auto out = resp->at("output").get<std::string>();
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        EXPECT_EQ(out, "True True True")
            << "Path keys should be non-empty, got: " << out;
    }

    // Verify broker operational keys
    {
        auto resp = hub.exec("c = pylabhub.config()\n"
                             "print(c['channel_timeout_s'], c['consumer_liveness_check_s'])");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
        auto out = resp->at("output").get<std::string>();
        EXPECT_NE(out.find("10"), std::string::npos)
            << "channel_timeout_s should be 10, got: " << out;
        EXPECT_NE(out.find("5"), std::string::npos)
            << "consumer_liveness_check_s should be 5, got: " << out;
    }

    EXPECT_EQ(hub.shutdown(), 0);
    fs::remove_all(tmp);
}

TEST_F(AdminShellTest, Probe_ChannelsEmpty)
{
    const auto ports = allocate_ports();
    const auto tmp = unique_temp_dir("probe_ch_empty");
    write_hub_json(tmp, ports);
    TLOG("TEST", "Probe_ChannelsEmpty: ports=({},{})", ports.broker, ports.admin);

    HubShellProcess hub(tmp, ports, "HUB9");
    ASSERT_TRUE(hub.valid());
    ASSERT_TRUE(hub.wait_ready());

    // No producers running → channels() should return empty categories.
    {
        auto resp = hub.exec("chs = pylabhub.channels()\n"
                             "print(len(chs['all']), len(chs['ready']), "
                             "len(chs['pending']), len(chs['closing']))");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
        auto out = resp->at("output").get<std::string>();
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        EXPECT_EQ(out, "0 0 0 0") << "Expected empty channels, got: " << out;
    }

    // Filter shortcut: channels('ready') should return empty list
    {
        auto resp = hub.exec("print(len(pylabhub.channels('ready')))");
        ASSERT_TRUE(resp.has_value());
        EXPECT_TRUE(resp->at("success").get<bool>());
        auto out = resp->at("output").get<std::string>();
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        EXPECT_EQ(out, "0");
    }

    // Invalid filter should raise ValueError
    {
        auto resp = hub.exec("pylabhub.channels('invalid')");
        ASSERT_TRUE(resp.has_value());
        EXPECT_FALSE(resp->at("success").get<bool>());
        auto err = resp->at("error").get<std::string>();
        EXPECT_NE(err.find("Unknown filter"), std::string::npos)
            << "Expected ValueError for invalid filter, got: " << err;
    }

    EXPECT_EQ(hub.shutdown(), 0);
    fs::remove_all(tmp);
}
