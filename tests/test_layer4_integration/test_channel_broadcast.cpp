/**
 * @file test_channel_broadcast.cpp
 * @brief Integration tests for channel broadcast, notify, and list_channels APIs.
 *
 * Tests the three new user-facing control-plane APIs:
 * - `api.list_channels()` — query broker for registered channels
 * - `api.broadcast_channel(target, message, data)` — send to ALL channel members
 * - `api.notify_channel(target, event, data)` — send to producer only
 *
 * Also tests admin shell functions:
 * - `pylabhub.broadcast_channel(channel, message, [data])`
 * - `pylabhub.close_channel(channel)`
 *
 * Pattern 3 (IsolatedProcessTest) — each test spawns live binaries.
 */

#include "test_patterns.h"
#include "test_process_utils.h"
#include "test_entrypoint.h"

#include <cppzmq/zmq.hpp>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <chrono>
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
                  .count() % 100000;
    return fmt::format("{:05d}", ms);
}

#define TLOG(tag, msg, ...) \
    fmt::print(stderr, "[{}][{}] " msg "\n", now_ms(), tag, ##__VA_ARGS__)

// ── Port allocation (PID-based, unique range for this test file) ─────────────

static std::atomic<int> s_bcast_port_counter{0};

struct BcastPorts
{
    int broker;
    int admin;
};

static BcastPorts allocate_bcast_ports()
{
    // Port range: 16000–18999 (broadcast tests).
    // Non-overlapping with admin (10000–12999) and pipeline (13000–15999).
    // pid%500*6 → max offset 2994; with slot*2 → max port 18999 < 65535.
    // L3 ZmqQueue tests use 33000+, so no cross-binary collision possible.
    const int pid_offset = (::getpid() % 500) * 6;
    constexpr int kBasePort = 16000;
    const int slot = s_bcast_port_counter.fetch_add(1);
    return {kBasePort + pid_offset + slot * 2, kBasePort + pid_offset + slot * 2 + 1};
}

// ── Binary path helpers ──────────────────────────────────────────────────────

static fs::path bin_dir()
{
    return fs::path(g_self_exe_path).parent_path() / ".." / "bin";
}

static std::string hubshell_binary()  { return (bin_dir() / "pylabhub-hubshell").string(); }
static std::string producer_binary()  { return (bin_dir() / "pylabhub-producer").string(); }
static std::string consumer_binary()  { return (bin_dir() / "pylabhub-consumer").string(); }

// ── Temp directory helper ────────────────────────────────────────────────────

static fs::path unique_temp_dir(const std::string& prefix)
{
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1);
    fs::path dir = fs::temp_directory_path()
                   / ("plh_bcast_" + prefix + "_" + std::to_string(id) + "_"
                      + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir;
}

// ── SHM secret constants ────────────────────────────────────────────────────

static constexpr uint64_t kShmSecret = 7780001;

// ── Config writers ──────────────────────────────────────────────────────────

static void write_hub_config(const fs::path& dir, const BcastPorts& ports)
{
    const json hub_json = {
        {"hub", {
            {"name",            "test-bcast-hub"},
            {"uid",             "HUB-BCAST-00000001"},
            {"broker_endpoint", fmt::format("tcp://127.0.0.1:{}", ports.broker)},
            {"admin_endpoint",  fmt::format("tcp://127.0.0.1:{}", ports.admin)}
        }},
        {"broker", {
            {"channel_timeout_s",          10},
            {"consumer_liveness_check_s",  5},
            {"channel_shutdown_grace_s",   1}
        }},
        {"script", {
            {"type",                   "python"},
            {"path",                   "."},
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

static json make_slot_schema()
{
    return {
        {"packing", "aligned"},
        {"fields", json::array({
            {{"name", "counter"}, {"type", "int64"}},
            {{"name", "value"},   {"type", "float64"}}
        })}
    };
}

static void write_producer_config(const fs::path& dir, const fs::path& hub_dir,
                                  const std::string& channel,
                                  const std::string& script)
{
    const json cfg = {
        {"hub_dir", hub_dir.string()},
        {"producer", {
            {"uid",       "PROD-BCAST-00000001"},
            {"name",      "BcastTestProducer"},
            {"log_level", "info"},
            {"auth",      {{"keyfile", ""}}}
        }},
        {"channel",     channel},
        {"target_period_ms", 200},   // 5 Hz — slow, just needs to keep alive
        {"shm", {
            {"enabled",    true},
            {"secret",     kShmSecret},
            {"slot_count", 8}
        }},
        {"slot_schema",     make_slot_schema()},
        {"flexzone_schema", nullptr},
        {"script", {{"type", "python"}, {"path", "."}}},
        {"validation", {
            {"update_checksum", true},
            {"stop_on_script_error", true}
        }}
    };

    fs::create_directories(dir / "script" / "python");
    fs::create_directories(dir / "logs");

    {
        std::ofstream f(dir / "script" / "python" / "__init__.py");
        f << script;
    }
    {
        std::ofstream f(dir / "producer.json");
        f << cfg.dump(2) << '\n';
    }
}

static void write_consumer_config(const fs::path& dir, const fs::path& hub_dir,
                                  const std::string& channel,
                                  const std::string& script,
                                  int timeout_ms = 2000)
{
    const json cfg = {
        {"hub_dir", hub_dir.string()},
        {"consumer", {
            {"uid",       "CONS-BCAST-00000002"},
            {"name",      "BcastTestConsumer"},
            {"log_level", "info"},
            {"auth",      {{"keyfile", ""}}}
        }},
        {"channel",    channel},
        {"slot_acquire_timeout_ms", timeout_ms},
        {"shm", {
            {"enabled", true},
            {"secret",  kShmSecret}
        }},
        {"slot_schema",     make_slot_schema()},
        {"flexzone_schema", nullptr},
        {"script", {{"type", "python"}, {"path", "."}}},
        {"validation", {
            {"stop_on_script_error", true}
        }}
    };

    fs::create_directories(dir / "script" / "python");
    fs::create_directories(dir / "logs");

    {
        std::ofstream f(dir / "script" / "python" / "__init__.py");
        f << script;
    }
    {
        std::ofstream f(dir / "consumer.json");
        f << cfg.dump(2) << '\n';
    }
}

// ── Admin shell helpers ─────────────────────────────────────────────────────

/// RAII wrapper for hub + admin ZMQ connection. Handles startup, exec, shutdown.
class TestHub
{
  public:
    TestHub(const fs::path& hub_dir, const BcastPorts& ports, const std::string& tag)
        : tag_(tag)
        , hub_dir_(hub_dir)
        , ports_(ports)
        , ctx_(1)
        , sock_(ctx_, zmq::socket_type::req)
        , proc_(hubshell_binary(), hub_dir.string(), {"--dev"},
                /*redirect_stderr_to_console=*/true)
    {
        sock_.set(zmq::sockopt::linger, 0);
        sock_.set(zmq::sockopt::rcvtimeo, 5000);
    }

    ~TestHub()
    {
        if (!shut_down_)
        {
            try { shutdown(); }
            catch (...) {}
        }
    }

    TestHub(const TestHub&) = delete;
    TestHub& operator=(const TestHub&) = delete;

    /// Wait for hub.pubkey + admin shell responsiveness.
    bool wait_ready(int timeout_s = 15)
    {
        if (!proc_.valid()) return false;

        // Wait for hub.pubkey
        const auto pubkey_path = hub_dir_ / "hub.pubkey";
        const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
        while (!fs::exists(pubkey_path) && std::chrono::steady_clock::now() < dl)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!fs::exists(pubkey_path)) return false;

        // Connect and probe admin shell
        sock_.connect(fmt::format("tcp://127.0.0.1:{}", ports_.admin));
        while (std::chrono::steady_clock::now() < dl)
        {
            json req{{"code", "pass"}};
            sock_.send(zmq::buffer(req.dump()), zmq::send_flags::none);
            zmq::message_t msg;
            auto rc = sock_.recv(msg, zmq::recv_flags::none);
            if (rc.has_value()) return true;
            // REQ socket enters bad state after recv timeout — must recreate.
            sock_.close();
            sock_ = zmq::socket_t(ctx_, zmq::socket_type::req);
            sock_.set(zmq::sockopt::linger, 0);
            sock_.set(zmq::sockopt::rcvtimeo, 5000);
            sock_.connect(fmt::format("tcp://127.0.0.1:{}", ports_.admin));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }

    /// Execute Python code on admin shell. Returns output string (empty on error).
    std::string exec(const std::string& code)
    {
        json req{{"code", code}};
        sock_.send(zmq::buffer(req.dump()), zmq::send_flags::none);
        zmq::message_t msg;
        auto rc = sock_.recv(msg, zmq::recv_flags::none);
        if (!rc.has_value())
        {
            TLOG(tag_, "exec timeout: '{}'", code.substr(0, 60));
            // Recreate socket after timeout
            sock_.close();
            sock_ = zmq::socket_t(ctx_, zmq::socket_type::req);
            sock_.set(zmq::sockopt::linger, 0);
            sock_.set(zmq::sockopt::rcvtimeo, 5000);
            sock_.connect(fmt::format("tcp://127.0.0.1:{}", ports_.admin));
            return {};
        }
        try
        {
            auto resp = json::parse(
                std::string(static_cast<const char*>(msg.data()), msg.size()));
            if (!resp.value("success", false))
            {
                TLOG(tag_, "exec error: {}", resp.value("error", "unknown"));
                return {};
            }
            return resp.value("output", "");
        }
        catch (...) { return {}; }
    }

    /// Poll until channel appears in ready list with min_consumers.
    bool wait_for_channel(const std::string& name, int min_consumers = 0,
                          int timeout_s = 20)
    {
        const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
        const std::string code = fmt::format(
            "chs = pylabhub.channels('ready')\n"
            "for ch in chs:\n"
            "    if ch['name'] == '{}':\n"
            "        print(f\"{{ch['name']}}:{{ch['consumer_count']}}\")\n"
            "        break\n"
            "else:\n"
            "    print('NOT_FOUND')\n",
            name);
        while (std::chrono::steady_clock::now() < dl)
        {
            const auto out = exec(code);
            if (!out.empty() && out.find("NOT_FOUND") == std::string::npos)
            {
                auto colon = out.find(':');
                if (colon != std::string::npos)
                {
                    int cc = std::stoi(out.substr(colon + 1));
                    if (cc >= min_consumers)
                        return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        return false;
    }

    int shutdown()
    {
        exec("pylabhub.shutdown()");
        sock_.close();
        ctx_.close();
        int rc = proc_.wait_for_exit();
        shut_down_ = true;
        return rc;
    }

    bool valid() const { return proc_.valid(); }

  private:
    std::string    tag_;
    fs::path       hub_dir_;
    BcastPorts     ports_;
    zmq::context_t ctx_;
    zmq::socket_t  sock_;
    WorkerProcess  proc_;
    bool           shut_down_ = false;
};

// ── Test fixture ─────────────────────────────────────────────────────────────

class ChannelBroadcastTest : public pylabhub::tests::IsolatedProcessTest {};

// ── Test 2a: list_channels returns accurate data ─────────────────────────────
//
// Producer calls api.list_channels() in on_produce and prints the result to stdout.
// We verify: channel exists, name correct, status ok.

TEST_F(ChannelBroadcastTest, ListChannels_Accurate)
{
    const auto ports    = allocate_bcast_ports();
    const auto hub_dir  = unique_temp_dir("lc_hub");
    const auto prod_dir = unique_temp_dir("lc_prod");

    write_hub_config(hub_dir, ports);

    // Producer script: on first iteration, call list_channels() and write results
    // to a result file. Tests should not rely on stdout capture from embedded
    // Python — use file-based or api.log()-based verification instead.
    const auto result_file = prod_dir / "result.txt";
    const std::string prod_script = fmt::format(R"py(
_checked = False

def on_init(api):
    api.log('info', 'ListChannels producer on_init')

def on_produce(out_slot, flexzone, messages, api):
    global _checked
    for m in messages:
        if isinstance(m, dict) and m.get('event') == 'channel_closing':
            api.stop()
            return False
    if _checked:
        return False
    _checked = True
    channels = api.list_channels()
    lines = []
    for ch in channels:
        name = ch.get('name', 'UNKNOWN')
        status = ch.get('status', 'UNKNOWN')
        cc = ch.get('consumer_count', -1)
        lines.append(f'CH:{{name}}:{{status}}:{{cc}}')
    if not channels:
        lines.append('CH:EMPTY')
    with open(r'{}', 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return False

def on_stop(api):
    pass
)py", result_file.string());

    write_producer_config(prod_dir, hub_dir, "test.lc.raw", prod_script);

    TLOG("TEST", "ListChannels: ports=({},{})", ports.broker, ports.admin);

    TestHub hub(hub_dir, ports, "HUB");
    ASSERT_TRUE(hub.valid());
    ASSERT_TRUE(hub.wait_ready());

    WorkerProcess producer(producer_binary(), prod_dir.string(), {},
                           /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(producer.valid());

    // Wait for channel to become Ready.
    ASSERT_TRUE(hub.wait_for_channel("test.lc.raw"))
        << "Channel 'test.lc.raw' never became Ready";

    // Give the producer a few iterations to call list_channels().
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Shut down gracefully.
    hub.exec("pylabhub.close_channel('test.lc.raw')");
    (void)producer.wait_for_exit();
    hub.shutdown();

    // Read the result file written by the producer script.
    ASSERT_TRUE(fs::exists(result_file))
        << "Producer did not write result file — list_channels() may not have been called";

    std::string result;
    {
        std::ifstream ifs(result_file);
        result.assign(std::istreambuf_iterator<char>(ifs),
                      std::istreambuf_iterator<char>());
    }
    TLOG("TEST", "ListChannels: result.txt:\n{}", result);

    ASSERT_FALSE(result.empty())
        << "Result file exists but is empty — script wrote nothing";

    // Verify: should contain "CH:test.lc.raw:Ready:0" (0 consumers).
    EXPECT_NE(result.find("CH:test.lc.raw:"), std::string::npos)
        << "list_channels() did not return producer's own channel. result:\n" << result;

    // Check it's not empty
    EXPECT_EQ(result.find("CH:EMPTY"), std::string::npos)
        << "list_channels() returned empty. result:\n" << result;

    fs::remove_all(hub_dir);
    fs::remove_all(prod_dir);
}

// ── Test 2b: broadcast_channel delivery to all members ───────────────────────
//
// Admin shell broadcasts a message to a channel; both producer and consumer
// should receive it and print confirmation to stdout.

TEST_F(ChannelBroadcastTest, BroadcastDelivery_AllMembers)
{
    const auto ports    = allocate_bcast_ports();
    const auto hub_dir  = unique_temp_dir("bc_hub");
    const auto prod_dir = unique_temp_dir("bc_prod");
    const auto cons_dir = unique_temp_dir("bc_cons");

    write_hub_config(hub_dir, ports);

    const auto prod_result = prod_dir / "result.txt";
    const auto cons_result = cons_dir / "result.txt";

    // Producer: checks messages for broadcast; if received, writes result and stops.
    const std::string prod_script = fmt::format(R"py(
def on_init(api):
    api.log('info', 'Broadcast producer on_init')

def on_produce(out_slot, flexzone, messages, api):
    for m in messages:
        if isinstance(m, dict) and m.get('message') == 'hello':
            with open(r'{}', 'w') as f:
                f.write('PROD_BCAST_OK\n')
            api.stop()
            return False
    return False

def on_stop(api):
    pass
)py", prod_result.string());

    // Consumer: if broadcast received, calls api.stop() → exit code 0 proves receipt.
    const std::string cons_script = fmt::format(R"py(
def on_init(api):
    api.log('info', 'Broadcast consumer on_init')

def on_consume(in_slot, flexzone, messages, api):
    for m in messages:
        api.log('info', f'CONS_MSG: {{type(m).__name__}} {{repr(m)[:200]}}')
        if isinstance(m, dict) and m.get('message') == 'hello':
            with open(r'{}', 'w') as f:
                f.write('CONS_BCAST_OK\n')
            api.stop()
            return

def on_stop(api):
    pass
)py", cons_result.string());

    write_producer_config(prod_dir, hub_dir, "test.bcast", prod_script);
    write_consumer_config(cons_dir, hub_dir, "test.bcast", cons_script);

    TLOG("TEST", "BroadcastDelivery: ports=({},{})", ports.broker, ports.admin);

    TestHub hub(hub_dir, ports, "HUB");
    ASSERT_TRUE(hub.valid());
    ASSERT_TRUE(hub.wait_ready());

    WorkerProcess producer(producer_binary(), prod_dir.string(), {},
                           /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(producer.valid());

    ASSERT_TRUE(hub.wait_for_channel("test.bcast"))
        << "Channel 'test.bcast' never became Ready";

    WorkerProcess consumer(consumer_binary(), cons_dir.string(), {},
                           /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(consumer.valid());

    ASSERT_TRUE(hub.wait_for_channel("test.bcast", /*min_consumers=*/1))
        << "Consumer never connected";

    // Small delay for ZMQ subscription propagation.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Broadcast via admin shell.
    TLOG("TEST", "BroadcastDelivery: broadcasting 'hello' with payload 'world'");
    hub.exec("pylabhub.broadcast_channel('test.bcast', 'hello', 'world')");

    // Both should self-stop on broadcast receipt.
    int cons_rc = consumer.wait_for_exit();
    TLOG("TEST", "BroadcastDelivery: consumer exited rc={}", cons_rc);

    // Producer should also self-stop. If it doesn't exit in time, close the channel.
    int prod_rc = producer.wait_for_exit();
    TLOG("TEST", "BroadcastDelivery: producer exited rc={}", prod_rc);
    hub.shutdown();

    // Verify receipt through result files (written by scripts on broadcast).
    ASSERT_TRUE(fs::exists(prod_result))
        << "Producer did not write result file — broadcast not received";
    ASSERT_TRUE(fs::exists(cons_result))
        << "Consumer did not write result file — broadcast not received";

    // Validate file content — must contain the expected markers.
    {
        std::string prod_out, cons_out;
        {
            std::ifstream ifs(prod_result);
            prod_out.assign(std::istreambuf_iterator<char>(ifs),
                            std::istreambuf_iterator<char>());
        }
        {
            std::ifstream ifs(cons_result);
            cons_out.assign(std::istreambuf_iterator<char>(ifs),
                            std::istreambuf_iterator<char>());
        }
        TLOG("TEST", "BroadcastDelivery: prod_result:\n{}", prod_out);
        TLOG("TEST", "BroadcastDelivery: cons_result:\n{}", cons_out);

        EXPECT_NE(prod_out.find("PROD_BCAST_OK"), std::string::npos)
            << "Producer result file missing PROD_BCAST_OK. Content:\n" << prod_out;
        EXPECT_NE(cons_out.find("CONS_BCAST_OK"), std::string::npos)
            << "Consumer result file missing CONS_BCAST_OK. Content:\n" << cons_out;
    }

    fs::remove_all(hub_dir);
    fs::remove_all(prod_dir);
    fs::remove_all(cons_dir);
}

// ── Test 2c: notify_channel delivery to producer only ────────────────────────
//
// Consumer calls api.notify_channel() targeting the same channel's producer.
// Producer should receive it; consumer should NOT.
//
// Test design:
//   - No data flow: producer always returns false (discard). This test is
//     purely about control-plane notify routing, not data-plane behavior.
//   - Consumer sends api.notify_channel('test.notify', 'test_ping') on first call.
//   - Producer checks messages for the 'test_ping' event.
//   - Consumer verifies it does NOT receive its own notify (events are
//     routed to the channel's producer, not back to the sender).
//   - Consumer uses time-based exit (2s wall clock) instead of iteration count
//     because the unified data loop runs MaxRate at 10μs per cycle —
//     iteration-count-based exits are timing-sensitive and unreliable.
//   - Producer exits via CHANNEL_CLOSING_NOTIFY when the test closes the channel.

TEST_F(ChannelBroadcastTest, NotifyChannel_ProducerOnly)
{
    const auto ports    = allocate_bcast_ports();
    const auto hub_dir  = unique_temp_dir("nc_hub");
    const auto prod_dir = unique_temp_dir("nc_prod");
    const auto cons_dir = unique_temp_dir("nc_cons");

    write_hub_config(hub_dir, ports);

    const auto prod_result = prod_dir / "result.txt";
    const auto cons_result = cons_dir / "result.txt";

    // Producer: checks messages for notify event, writes confirmation to file.
    const std::string prod_script = fmt::format(R"py(
def on_init(api):
    api.log('info', 'Notify producer on_init')

def on_produce(out_slot, flexzone, messages, api):
    for m in messages:
        if isinstance(m, dict) and m.get('event') == 'test_ping':
            with open(r'{}', 'w') as f:
                f.write(f"PROD_NOTIFY:test_ping:{{m.get('sender_uid', 'none')}}\n")
        elif isinstance(m, dict) and m.get('event') == 'channel_closing':
            api.stop()
            return False
    return False

def on_stop(api):
    pass
)py", prod_result.string());

    // Consumer: sends notify on first iteration, then checks it does NOT
    // receive the notify event itself. Writes result to file.
    const std::string cons_script = fmt::format(R"py(
import time as _time
_sent = False
_start_time = None
_got_notify = False

def on_init(api):
    api.log('info', 'Notify consumer on_init')

def on_consume(in_slot, flexzone, messages, api):
    global _sent, _start_time, _got_notify

    if _start_time is None:
        _start_time = _time.monotonic()

    # Process control messages.
    for m in messages:
        if isinstance(m, dict) and m.get('event') == 'test_ping':
            _got_notify = True

    # Send notify on first opportunity.
    if not _sent:
        api.notify_channel('test.notify', 'test_ping')
        _sent = True
        api.log('info', 'Consumer: sent notify')

    # Wait 2 seconds (enough time for broker to route the notify),
    # then write result and stop.
    if _time.monotonic() - _start_time >= 2.0:
        with open(r'{}', 'w') as f:
            if _got_notify:
                f.write('CONS_GOT_NOTIFY:UNEXPECTED\n')
            else:
                f.write('CONS_DONE:no_notify_received\n')
        api.stop()

def on_stop(api):
    pass
)py", cons_result.string());

    write_producer_config(prod_dir, hub_dir, "test.notify", prod_script);
    write_consumer_config(cons_dir, hub_dir, "test.notify", cons_script,
                          /*timeout_ms=*/200);

    TLOG("TEST", "NotifyChannel: ports=({},{})", ports.broker, ports.admin);

    TestHub hub(hub_dir, ports, "HUB");
    ASSERT_TRUE(hub.valid());
    ASSERT_TRUE(hub.wait_ready());

    WorkerProcess producer(producer_binary(), prod_dir.string(), {},
                           /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(producer.valid());

    ASSERT_TRUE(hub.wait_for_channel("test.notify"))
        << "Channel 'test.notify' never became Ready";

    WorkerProcess consumer(consumer_binary(), cons_dir.string(), {},
                           /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(consumer.valid());

    ASSERT_TRUE(hub.wait_for_channel("test.notify", /*min_consumers=*/1))
        << "Consumer never connected";

    // Wait for consumer to complete its iterations and stop.
    int cons_rc = consumer.wait_for_exit();
    TLOG("TEST", "NotifyChannel: consumer exited rc={}", cons_rc);

    // Give producer time to receive the notify.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    hub.exec("pylabhub.close_channel('test.notify')");
    (void)producer.wait_for_exit();
    hub.shutdown();

    // Both result files MUST exist — scripts write them as proof of execution.
    ASSERT_TRUE(fs::exists(prod_result))
        << "Producer did not write result file — notify may not have been received";
    ASSERT_TRUE(fs::exists(cons_result))
        << "Consumer did not write result file — script did not complete 8 iterations";

    // Read result files.
    std::string prod_out, cons_out;
    {
        std::ifstream ifs(prod_result);
        prod_out.assign(std::istreambuf_iterator<char>(ifs),
                        std::istreambuf_iterator<char>());
    }
    {
        std::ifstream ifs(cons_result);
        cons_out.assign(std::istreambuf_iterator<char>(ifs),
                        std::istreambuf_iterator<char>());
    }
    TLOG("TEST", "NotifyChannel: prod_result:\n{}", prod_out);
    TLOG("TEST", "NotifyChannel: cons_result:\n{}", cons_out);

    // Guard: files must not be empty (script logic always writes content).
    ASSERT_FALSE(prod_out.empty())
        << "Producer result file exists but is empty";
    ASSERT_FALSE(cons_out.empty())
        << "Consumer result file exists but is empty";

    // Producer SHOULD have received the notify event.
    EXPECT_NE(prod_out.find("PROD_NOTIFY:test_ping:"), std::string::npos)
        << "Producer did not receive notify. result:\n" << prod_out;

    // Consumer should NOT have received the notify.
    EXPECT_EQ(cons_out.find("CONS_GOT_NOTIFY"), std::string::npos)
        << "Consumer unexpectedly received notify. result:\n" << cons_out;

    // Consumer should have completed normally.
    EXPECT_NE(cons_out.find("CONS_DONE:no_notify_received"), std::string::npos)
        << "Consumer did not complete. result:\n" << cons_out;

    fs::remove_all(hub_dir);
    fs::remove_all(prod_dir);
    fs::remove_all(cons_dir);
}

// ── Test 2d: Admin broadcast + close_channel graceful shutdown ───────────────
//
// Admin sends broadcast to producer; producer prints confirmation.
// Admin then calls close_channel; producer exits gracefully.

TEST_F(ChannelBroadcastTest, AdminBroadcast_CloseChannel)
{
    const auto ports    = allocate_bcast_ports();
    const auto hub_dir  = unique_temp_dir("ac_hub");
    const auto prod_dir = unique_temp_dir("ac_prod");

    write_hub_config(hub_dir, ports);

    const auto prod_result = prod_dir / "result.txt";
    const auto prod_bcast_ack = prod_dir / "bcast_received.txt";

    // Producer: checks messages for admin broadcast.
    // Immediately writes an ack file when broadcast received (for test synchronization).
    // on_stop writes final result file.
    const std::string prod_script = fmt::format(R"py(
_results = []

def on_init(api):
    api.log('info', 'AdminCtrl producer on_init')

def on_produce(out_slot, flexzone, messages, api):
    for m in messages:
        if isinstance(m, dict) and m.get('message') == 'admin_ping':
            _results.append('PROD_ADMIN:admin_ping')
            # Immediately write ack so test knows broadcast was processed.
            with open(r'{}', 'w') as f:
                f.write('OK\n')
        elif isinstance(m, dict) and m.get('event') == 'channel_closing':
            api.stop()
            return False
    return False

def on_stop(api):
    _results.append('PROD_STOPPED')
    with open(r'{}', 'w') as f:
        f.write('\n'.join(_results) + '\n')
)py", prod_bcast_ack.string(), prod_result.string());

    write_producer_config(prod_dir, hub_dir, "test.ctrl", prod_script);

    TLOG("TEST", "AdminBroadcast_CloseChannel: ports=({},{})", ports.broker, ports.admin);

    TestHub hub(hub_dir, ports, "HUB");
    ASSERT_TRUE(hub.valid());
    ASSERT_TRUE(hub.wait_ready());

    WorkerProcess producer(producer_binary(), prod_dir.string(), {},
                           /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(producer.valid());

    ASSERT_TRUE(hub.wait_for_channel("test.ctrl"))
        << "Channel 'test.ctrl' never became Ready";

    // Small delay for ZMQ propagation.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Admin broadcast.
    hub.exec("pylabhub.broadcast_channel('test.ctrl', 'admin_ping')");

    // Wait for producer to acknowledge the broadcast (deterministic sync,
    // avoids the race where close_channel arrives before the broadcast
    // is drained from the message queue).
    {
        const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fs::exists(prod_bcast_ack) && std::chrono::steady_clock::now() < dl)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(fs::exists(prod_bcast_ack))
        << "Producer did not acknowledge broadcast within timeout";
    TLOG("TEST", "AdminBroadcast_CloseChannel: broadcast acknowledged by producer");

    // Close channel — producer should shut down gracefully.
    TLOG("TEST", "AdminBroadcast_CloseChannel: closing channel");
    hub.exec("pylabhub.close_channel('test.ctrl')");
    int prod_rc = producer.wait_for_exit();
    TLOG("TEST", "AdminBroadcast_CloseChannel: producer exited rc={}", prod_rc);

    hub.shutdown();

    // Result file MUST exist — on_stop always writes it.
    ASSERT_TRUE(fs::exists(prod_result))
        << "Producer did not write result file — on_stop may not have been called";

    std::string prod_out;
    {
        std::ifstream ifs(prod_result);
        prod_out.assign(std::istreambuf_iterator<char>(ifs),
                        std::istreambuf_iterator<char>());
    }
    TLOG("TEST", "AdminBroadcast_CloseChannel: prod_result:\n{}", prod_out);

    ASSERT_FALSE(prod_out.empty())
        << "Producer result file exists but is empty";

    // Producer received the admin broadcast.
    EXPECT_NE(prod_out.find("PROD_ADMIN:admin_ping"), std::string::npos)
        << "Producer did not receive admin broadcast. result:\n" << prod_out;

    // Producer stopped gracefully (on_stop was called).
    EXPECT_NE(prod_out.find("PROD_STOPPED"), std::string::npos)
        << "Producer on_stop not called. result:\n" << prod_out;

    // Exit code 0 = clean shutdown via CHANNEL_CLOSING_NOTIFY.
    EXPECT_EQ(prod_rc, 0) << "Producer did not exit cleanly";

    fs::remove_all(hub_dir);
    fs::remove_all(prod_dir);
}
