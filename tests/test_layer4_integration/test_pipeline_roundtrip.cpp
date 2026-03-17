/**
 * @file test_pipeline_roundtrip.cpp
 * @brief Integration test for the full pipeline: Producer → Processor → Consumer.
 *
 * Spawns all four binaries (hubshell, producer, processor, consumer) and verifies
 * that data flows end-to-end through the pipeline with correct transformation.
 *
 * Pattern 3 (IsolatedProcessTest) — subprocess-per-test.
 *
 * Pipeline topology:
 *   Producer (test.pipe.raw) → Processor → (test.pipe.out) → Consumer
 *
 * ## Synchronization model (broadcast-based)
 *
 * All roles start in "suspended" mode — the script loop runs but does not produce
 * or consume data until a "start" broadcast is received via the broker.
 *
 * 1. Test starts hubshell, producer, processor, consumer (in order).
 * 2. Test polls admin shell until both channels (test.pipe.raw, test.pipe.out) are
 *    Ready AND each has at least 1 consumer registered.
 * 3. Test broadcasts "start" to BOTH channels via `pylabhub.broadcast_channel()`.
 * 4. All roles receive the "start" broadcast in their `msgs` list and begin
 *    producing/processing/consuming data.
 * 5. Producer produces CONTINUOUSLY (never self-stops). This exercises the full
 *    ring-buffer / queue mechanism under sustained load.
 * 6. Consumer stops itself after receiving kTargetSlots data slots.
 * 7. Consumer's exit is the natural termination signal.
 * 8. Test uses `pylabhub.close_channel()` to gracefully shut down remaining roles.
 *
 * ## Why broadcast synchronization?
 *
 * Without synchronization, the producer may write many SHM slots before downstream
 * roles connect. With Latest_only consumer policy, early data is lost. By holding
 * all roles in "suspended" mode until the full pipeline is wired, we guarantee that
 * every slot produced after the "start" signal reaches the consumer.
 */

#include "test_patterns.h"
#include "test_process_utils.h"
#include "test_entrypoint.h"

#include <cppzmq/zmq.hpp>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
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

// ── Port allocation (PID-based to avoid parallel CTest collisions) ───────────

static std::atomic<int> s_pipeline_port_counter{0};

struct PipelinePorts
{
    int broker;
    int admin;
};

static PipelinePorts allocate_pipeline_ports()
{
    // Port range: 13000–15999 (pipeline tests).
    // Non-overlapping with admin (10000–12999) and broadcast (16000–18999).
    // pid%500*6 → max offset 2994; with slot*2 → max port 15999 < 65535.
    // L3 ZmqQueue tests use 33000+, so no cross-binary collision possible.
    const int pid_offset = (::getpid() % 500) * 6;
    constexpr int kBasePort = 13000;
    const int slot = s_pipeline_port_counter.fetch_add(1);
    return {kBasePort + pid_offset + slot * 2, kBasePort + pid_offset + slot * 2 + 1};
}

// ── Binary path helpers ──────────────────────────────────────────────────────

static fs::path bin_dir()
{
    return fs::path(g_self_exe_path).parent_path() / ".." / "bin";
}

static std::string hubshell_binary()  { return (bin_dir() / "pylabhub-hubshell").string(); }
static std::string producer_binary()  { return (bin_dir() / "pylabhub-producer").string(); }
static std::string processor_binary() { return (bin_dir() / "pylabhub-processor").string(); }
static std::string consumer_binary()  { return (bin_dir() / "pylabhub-consumer").string(); }

// ── Temp directory helper ────────────────────────────────────────────────────

static fs::path unique_temp_dir(const std::string& prefix)
{
    static std::atomic<int> counter{0};
    const int id = counter.fetch_add(1);
    fs::path dir = fs::temp_directory_path()
                   / ("plh_pipe_" + prefix + "_" + std::to_string(id) + "_"
                      + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir;
}

// ── Config + script writers ──────────────────────────────────────────────────

static constexpr uint64_t kShmSecretA = 7770001;  // producer ↔ processor
static constexpr uint64_t kShmSecretB = 7770002;  // processor ↔ consumer
static constexpr int kTargetSlots = 10;            // consumer exits after this many

static void write_hub_config(const fs::path& dir, const PipelinePorts& ports)
{
    const json hub_json = {
        {"hub", {
            {"name",            "test-pipe-hub"},
            {"uid",             "HUB-PIPE-00000001"},
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

static void write_producer_config(const fs::path& dir, const fs::path& hub_dir)
{
    const json cfg = {
        {"hub_dir", hub_dir.string()},
        {"producer", {
            {"uid",       "PROD-PIPE-00000001"},
            {"name",      "PipeTestProducer"},
            {"log_level", "info"},
            {"auth",      {{"keyfile", ""}}}
        }},
        {"channel",     "test.pipe.raw"},
        {"target_period_ms", 50},  // 20 Hz — continuous high-frequency production
        {"shm", {
            {"enabled",    true},
            {"secret",     kShmSecretA},
            {"slot_count", 16}
        }},
        {"slot_schema", {
            {"packing", "aligned"},
            {"fields", json::array({
                {{"name", "counter"}, {"type", "int64"}},
                {{"name", "value"},   {"type", "float64"}}
            })}
        }},
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
        // Producer script: suspended until "start" broadcast, then produces continuously.
        // Stops on "stop" broadcast or on_channel_closing (handled by framework).
        std::ofstream f(dir / "script" / "python" / "__init__.py");
        f << R"py(
_running = False
_count = 0

def on_init(api):
    api.log('info', 'PipeTestProducer: on_init — waiting for start broadcast')

def on_produce(out_slot, flexzone, messages, api):
    global _running, _count
    # Check messages for broadcast events
    for m in messages:
        if isinstance(m, dict) and m.get('message') == 'start':
            _running = True
            api.log('info', 'PipeTestProducer: received start broadcast, producing data')
        elif isinstance(m, dict) and m.get('message') == 'stop':
            api.log('info', f'PipeTestProducer: received stop broadcast after {_count} slots')
            api.stop()
            return False
        elif isinstance(m, dict) and m.get('event') == 'channel_closing':
            api.log('info', f'PipeTestProducer: channel_closing after {_count} slots')
            api.stop()
            return False
    if not _running:
        return False  # Suspended: discard output slot, wait for broadcast
    if out_slot is None:
        return False
    _count += 1
    out_slot.counter = _count
    out_slot.value = float(_count) * 10.0
    return True

def on_stop(api):
    api.log('info', f'PipeTestProducer: stopped, total={_count}')
)py";
    }
    {
        std::ofstream f(dir / "producer.json");
        f << cfg.dump(2) << '\n';
    }
}

static void write_processor_config(const fs::path& dir, const fs::path& hub_dir)
{
    const json cfg = {
        {"processor", {
            {"uid",       "PROC-PIPE-00000002"},
            {"name",      "PipeTestProcessor"},
            {"log_level", "info"}
        }},
        {"in_channel",  "test.pipe.raw"},
        {"out_channel", "test.pipe.out"},
        {"hub_dir",     hub_dir.string()},
        {"slot_acquire_timeout_ms",  2000},
        {"overflow_policy", "drop"},
        {"shm", {
            {"in",  {{"enabled", true}, {"secret", kShmSecretA}}},
            {"out", {{"enabled", true}, {"secret", kShmSecretB}, {"slot_count", 16}}}
        }},
        {"in_slot_schema", {
            {"packing", "aligned"},
            {"fields", json::array({
                {{"name", "counter"}, {"type", "int64"}},
                {{"name", "value"},   {"type", "float64"}}
            })}
        }},
        {"out_slot_schema", {
            {"packing", "aligned"},
            {"fields", json::array({
                {{"name", "counter"}, {"type", "int64"}},
                {{"name", "doubled"}, {"type", "float64"}}
            })}
        }},
        {"flexzone_schema", nullptr},
        {"script", {{"type", "python"}, {"path", "."}}},
        {"validation", {
            {"update_checksum", true},
            {"stop_on_script_error", true}
        }},
        {"startup", {
            {"wait_for_roles", json::array({
                {{"uid", "PROD-PIPE-00000001"}, {"timeout_ms", 15000}}
            })}
        }}
    };

    fs::create_directories(dir / "script" / "python");
    fs::create_directories(dir / "logs");

    {
        // Processor: always active (no suspend). Doubles the value field.
        // Runs until channel closes or SIGTERM.
        std::ofstream f(dir / "script" / "python" / "__init__.py");
        f << R"py(
_processed = 0

def on_init(api):
    api.log('info', 'PipeTestProcessor: on_init — ready to process')

def on_process(in_slot, out_slot, flexzone, messages, api):
    global _processed
    for m in messages:
        if isinstance(m, dict) and m.get('event') == 'channel_closing':
            api.log('info', f'PipeTestProcessor: channel_closing after {_processed} slots')
            api.stop()
            return False
    if in_slot is None:
        return False  # Timeout: no data yet, wait
    _processed += 1
    out_slot.counter = in_slot.counter
    out_slot.doubled = in_slot.value * 2.0
    return True

def on_stop(api):
    api.log('info', f'PipeTestProcessor: stopped, processed={_processed}')
)py";
    }
    {
        std::ofstream f(dir / "processor.json");
        f << cfg.dump(2) << '\n';
    }
}

static void write_consumer_config(const fs::path& dir, const fs::path& hub_dir,
                                  const fs::path& result_file)
{
    const json cfg = {
        {"hub_dir", hub_dir.string()},
        {"consumer", {
            {"uid",       "CONS-PIPE-00000003"},
            {"name",      "PipeTestConsumer"},
            {"log_level", "info"},
            {"auth",      {{"keyfile", ""}}}
        }},
        {"channel",    "test.pipe.out"},
        {"slot_acquire_timeout_ms", 2000},
        {"shm", {
            {"enabled", true},
            {"secret",  kShmSecretB}
        }},
        {"slot_schema", {
            {"packing", "aligned"},
            {"fields", json::array({
                {{"name", "counter"}, {"type", "int64"}},
                {{"name", "doubled"}, {"type", "float64"}}
            })}
        }},
        {"flexzone_schema", nullptr},
        {"script", {{"type", "python"}, {"path", "."}}},
        {"validation", {
            {"stop_on_script_error", true}
        }},
        {"startup", {
            {"wait_for_roles", json::array({
                {{"uid", "PROC-PIPE-00000002"}, {"timeout_ms", 15000}}
            })}
        }}
    };

    fs::create_directories(dir / "script" / "python");
    fs::create_directories(dir / "logs");

    {
        // Consumer script: suspended until "start" broadcast, then consumes
        // kTargetSlots data slots and calls api.stop().
        // Writes "SLOT:<counter>:<doubled>" lines to a result file for verification.
        std::ofstream f(dir / "script" / "python" / "__init__.py");
        f << fmt::format(R"py(
_running = False
_received = 0
_slots = []

def on_init(api):
    api.log('info', 'PipeTestConsumer: on_init — waiting for start broadcast')

def on_consume(in_slot, flexzone, messages, api):
    global _running, _received
    for m in messages:
        if isinstance(m, dict) and m.get('message') == 'start':
            _running = True
            api.log('info', 'PipeTestConsumer: received start broadcast')
    if not _running:
        return
    if in_slot is None:
        return
    _received += 1
    _slots.append(f'SLOT:{{in_slot.counter}}:{{in_slot.doubled}}')
    if _received >= {}:
        api.log('info', f'PipeTestConsumer: target reached ({{_received}} slots), stopping')
        api.stop()

def on_stop(api):
    api.log('info', f'PipeTestConsumer: stopped, total={{_received}}')
    with open(r'{}', 'w') as f:
        f.write('\n'.join(_slots) + '\n')
)py", kTargetSlots, result_file.string());
    }
    {
        std::ofstream f(dir / "consumer.json");
        f << cfg.dump(2) << '\n';
    }
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class PipelineRoundtripTest : public pylabhub::tests::IsolatedProcessTest {};

// ── Full pipeline: Producer → Processor → Consumer ───────────────────────────

TEST_F(PipelineRoundtripTest, ProducerProcessorConsumer_E2E)
{
    const auto ports = allocate_pipeline_ports();
    const auto hub_dir  = unique_temp_dir("pipe_hub");
    const auto prod_dir = unique_temp_dir("pipe_prod");
    const auto proc_dir = unique_temp_dir("pipe_proc");
    const auto cons_dir = unique_temp_dir("pipe_cons");

    TLOG("TEST", "E2E: ports=({},{})", ports.broker, ports.admin);

    const auto cons_result = cons_dir / "result.txt";

    // Write configs and scripts
    write_hub_config(hub_dir, ports);
    write_producer_config(prod_dir, hub_dir);
    write_processor_config(proc_dir, hub_dir);
    write_consumer_config(cons_dir, hub_dir, cons_result);

    // =========================================================================
    // Phase 1: Start hub, wait for broker ready
    // =========================================================================

    TLOG("TEST", "E2E: starting hubshell --dev");
    WorkerProcess hub(hubshell_binary(), hub_dir.string(), {"--dev"},
                      /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(hub.valid()) << "Failed to spawn hubshell";

    // Wait for the hub to write hub.pubkey (broker on_ready callback).
    const auto pubkey_path = hub_dir / "hub.pubkey";
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fs::exists(pubkey_path) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(fs::exists(pubkey_path))
        << "hub.pubkey not written — broker did not start";
    TLOG("TEST", "E2E: hub.pubkey found, broker is ready");

    // Persistent admin shell connection for hub queries.
    zmq::context_t admin_ctx(1);
    zmq::socket_t  admin_sock(admin_ctx, zmq::socket_type::req);
    admin_sock.set(zmq::sockopt::linger, 0);
    admin_sock.set(zmq::sockopt::rcvtimeo, 5000);
    admin_sock.connect(fmt::format("tcp://127.0.0.1:{}", ports.admin));

    // Helper: execute Python code on the hub's admin shell and return the output.
    auto admin_exec = [&](const std::string& code) -> std::string
    {
        json req{{"code", code}};
        admin_sock.send(zmq::buffer(req.dump()), zmq::send_flags::none);

        zmq::message_t msg;
        auto rc = admin_sock.recv(msg, zmq::recv_flags::none);
        if (!rc.has_value())
            return {};
        try
        {
            auto resp = json::parse(
                std::string(static_cast<const char*>(msg.data()), msg.size()));
            if (!resp.value("success", false))
            {
                TLOG("ADMIN", "exec error: {}", resp.value("error", "unknown"));
                return {};
            }
            return resp.value("output", "");
        }
        catch (...) { return {}; }
    };

    // Wait for admin shell to be responsive.
    {
        const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool ready = false;
        while (std::chrono::steady_clock::now() < dl)
        {
            json req{{"code", "pass"}};
            admin_sock.send(zmq::buffer(req.dump()), zmq::send_flags::none);
            zmq::message_t msg;
            auto rc = admin_sock.recv(msg, zmq::recv_flags::none);
            if (rc.has_value())
            {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ASSERT_TRUE(ready) << "Admin shell not responsive";
        TLOG("TEST", "E2E: admin shell is responsive");
    }

    // Helper: poll hub via admin shell until a channel with the given name
    // appears in the ready channels list with at least min_consumers.
    auto wait_for_channel_ready = [&](const std::string& channel_name,
                                      int min_consumers = 0,
                                      int timeout_s = 20) -> bool
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
            channel_name);
        while (std::chrono::steady_clock::now() < dl)
        {
            const auto out = admin_exec(code);
            TLOG("TEST", "E2E: wait_for_channel('{}', min_cons={}) → '{}'",
                 channel_name, min_consumers, out);
            if (!out.empty() && out.find("NOT_FOUND") == std::string::npos)
            {
                // Parse "channel_name:consumer_count"
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
    };

    // =========================================================================
    // Phase 2: Start all roles (all start in suspended mode)
    // =========================================================================

    TLOG("TEST", "E2E: starting producer");
    WorkerProcess producer(producer_binary(), prod_dir.string(), {},
                           /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(producer.valid()) << "Failed to spawn producer";

    // Wait for test.pipe.raw to be Ready (producer registers it).
    ASSERT_TRUE(wait_for_channel_ready("test.pipe.raw"))
        << "Channel 'test.pipe.raw' never became Ready";
    TLOG("TEST", "E2E: channel 'test.pipe.raw' is Ready");

    TLOG("TEST", "E2E: starting processor");
    WorkerProcess processor(processor_binary(), proc_dir.string(), {},
                            /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(processor.valid()) << "Failed to spawn processor";

    // Wait for test.pipe.out to be Ready AND test.pipe.raw to have 1+ consumer (the processor).
    ASSERT_TRUE(wait_for_channel_ready("test.pipe.out"))
        << "Channel 'test.pipe.out' never became Ready";
    ASSERT_TRUE(wait_for_channel_ready("test.pipe.raw", /*min_consumers=*/1))
        << "Channel 'test.pipe.raw' has no consumers (processor not connected)";
    TLOG("TEST", "E2E: channel 'test.pipe.out' is Ready, processor connected to raw");

    TLOG("TEST", "E2E: starting consumer");
    WorkerProcess consumer(consumer_binary(), cons_dir.string(), {},
                           /*redirect_stderr_to_console=*/true);
    ASSERT_TRUE(consumer.valid()) << "Failed to spawn consumer";

    // Wait for test.pipe.out to have 1+ consumer (the consumer process).
    ASSERT_TRUE(wait_for_channel_ready("test.pipe.out", /*min_consumers=*/1))
        << "Channel 'test.pipe.out' has no consumers (consumer not connected)";
    TLOG("TEST", "E2E: all roles connected, pipeline fully wired");

    // Roles use startup.wait_for_roles for self-coordination, and
    // wait_for_channel_ready confirms from the test side. No sleep needed.

    // =========================================================================
    // Phase 3: Broadcast "start" to begin data flow
    // =========================================================================

    TLOG("TEST", "E2E: broadcasting 'start' to both channels");
    admin_exec("pylabhub.broadcast_channel('test.pipe.raw', 'start')");
    admin_exec("pylabhub.broadcast_channel('test.pipe.out', 'start')");
    TLOG("TEST", "E2E: start broadcast sent — data should now be flowing");

    // =========================================================================
    // Phase 4: Wait for consumer to exit (it self-stops after kTargetSlots)
    // =========================================================================

    TLOG("TEST", "E2E: waiting for consumer to exit (target={} slots)...", kTargetSlots);
    int cons_rc = consumer.wait_for_exit();
    TLOG("TEST", "E2E: consumer exited with code {}", cons_rc);

    // =========================================================================
    // Phase 5: Graceful shutdown via channel close
    // =========================================================================

    // Broadcast "stop" to producer so it exits its loop cleanly.
    TLOG("TEST", "E2E: broadcasting 'stop' to producer channel");
    admin_exec("pylabhub.broadcast_channel('test.pipe.raw', 'stop')");

    // Close both channels — sends CHANNEL_CLOSING_NOTIFY to all participants.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    admin_exec("pylabhub.close_channel('test.pipe.out')");
    admin_exec("pylabhub.close_channel('test.pipe.raw')");

    int prod_rc = producer.wait_for_exit();
    TLOG("TEST", "E2E: producer exited with code {}", prod_rc);

    int proc_rc = processor.wait_for_exit();
    TLOG("TEST", "E2E: processor exited with code {}", proc_rc);

    TLOG("TEST", "E2E: shutting down hub");
    admin_exec("pylabhub.shutdown()");
    admin_sock.close();
    admin_ctx.close();
    int hub_rc = hub.wait_for_exit();
    TLOG("TEST", "E2E: hub exited with code {}", hub_rc);

    // =========================================================================
    // Verify results
    // =========================================================================

    // All processes should exit cleanly.
    EXPECT_EQ(cons_rc, 0) << "Consumer did not exit cleanly. stderr:\n" << consumer.get_stderr();
    EXPECT_EQ(prod_rc, 0) << "Producer did not exit cleanly. stderr:\n" << producer.get_stderr();
    EXPECT_EQ(proc_rc, 0) << "Processor did not exit cleanly. stderr:\n" << processor.get_stderr();
    EXPECT_EQ(hub_rc, 0)  << "Hub did not exit cleanly. stderr:\n" << hub.get_stderr();

    // Read SLOT lines from result file (written by consumer's on_stop callback).
    ASSERT_TRUE(fs::exists(cons_result))
        << "Consumer result file not written: " << cons_result
        << "\nConsumer stderr:\n" << consumer.get_stderr();

    std::string result_data;
    {
        std::ifstream rf(cons_result);
        result_data.assign(std::istreambuf_iterator<char>(rf),
                           std::istreambuf_iterator<char>());
    }
    TLOG("TEST", "E2E: result file ({} bytes):\n{}", result_data.size(), result_data);

    // Parse SLOT:<counter>:<doubled> lines
    std::vector<std::pair<int64_t, double>> slots;
    std::istringstream iss(result_data);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.substr(0, 5) == "SLOT:")
        {
            auto first_colon = line.find(':', 5);
            if (first_colon != std::string::npos)
            {
                int64_t counter = std::stoll(line.substr(5, first_colon - 5));
                double doubled  = std::stod(line.substr(first_colon + 1));
                slots.emplace_back(counter, doubled);
            }
        }
    }

    TLOG("TEST", "E2E: parsed {} SLOT lines", slots.size());

    // Must have received at least kTargetSlots
    ASSERT_GE(slots.size(), static_cast<size_t>(kTargetSlots))
        << "Consumer did not receive enough slots. Result file:\n" << result_data
        << "\nConsumer stderr:\n" << consumer.get_stderr();

    // Verify transformation: doubled should be counter * 10.0 * 2.0
    // (producer writes value = counter * 10.0, processor doubles it)
    for (const auto& [counter, doubled] : slots)
    {
        double expected = static_cast<double>(counter) * 10.0 * 2.0;
        EXPECT_DOUBLE_EQ(doubled, expected)
            << "counter=" << counter << " doubled=" << doubled
            << " expected=" << expected;
    }

    // Counters should be monotonically increasing (continuous flow, no gaps under load)
    for (size_t i = 1; i < slots.size(); ++i)
    {
        EXPECT_GT(slots[i].first, slots[i - 1].first)
            << "Counters not monotonically increasing at index " << i;
    }

    TLOG("TEST", "E2E: verified {} slots, all transformations correct", slots.size());

    // Cleanup temp dirs
    fs::remove_all(hub_dir);
    fs::remove_all(prod_dir);
    fs::remove_all(proc_dir);
    fs::remove_all(cons_dir);

    TLOG("TEST", "E2E: PASS");
}
