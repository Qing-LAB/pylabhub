/**
 * @file test_plh_hub_role_roundtrip.cpp
 * @brief plh_hub + plh_role end-to-end broker round-trip
 *        (HUB_TEST_COVERAGE_PLAN slice 6 — F1 regression test).
 *
 * Spawns BOTH binaries as real subprocesses and verifies the
 * documented operator flow works without manual intervention:
 *
 *   1. plh_hub --init / --keygen / run produces a directory layout
 *      that contains both `vault/hub.vault` (CURVE keypair, encrypted)
 *      AND `hub.pubkey` (public Z85 key the role reads via HubRefConfig).
 *      F1's fix: HubVault::publish_public_key is now wired into
 *      HubConfig::create_keypair so --keygen actually produces both.
 *   2. plh_role producer reads hub.json (broker_endpoint) +
 *      hub.pubkey (broker_pubkey for CURVE pin) from the hub dir,
 *      connects to the broker, and registers a channel via REG_REQ.
 *   3. The broker logs "Broker: registered channel '<name>'" — the
 *      OUTPUT proof that the wire-protocol round-trip succeeded
 *      end-to-end.
 *   4. SIGTERM to both; both exit 0 with no stray ERROR-level lines.
 *
 * Test rigor:
 *
 * - Both binaries verified via OUTPUT (log markers) AND exit-code AND
 *   Class-D ERROR gate.  No "wait long enough and hope" — every
 *   wait_for_log_marker has a watchdog ceiling, and ceiling expiry
 *   = clean failure (broker never bound, role never registered, etc.).
 * - Tests run with CURVE auth ON (token_required=false because
 *   admin is disabled here, so loopback-only enforcement still
 *   holds — see HEP §11.3).  The CURVE pin propagates from
 *   hub.pubkey through HubRefConfig to BrokerRequestComm; this is
 *   the PATH F1's bug broke before the fix.
 */

#include "plh_hub_fixture.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <thread>

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

// Need access to the L3-style log-reading helpers; redefine the
// minimum here (the runmode test file's namespace versions are not
// visible across TUs).  Same shape, same behaviour.

std::string read_hub_log(const fs::path &hub_dir)
{
    const fs::path logs = hub_dir / "logs";
    std::error_code ec;
    if (!fs::is_directory(logs, ec)) return {};
    fs::path newest;
    for (const auto &e : fs::directory_iterator(logs, ec))
        if (e.path().extension() == ".log" && e.path() > newest)
            newest = e.path();
    if (newest.empty()) return {};
    std::ifstream f(newest);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>{});
}

bool wait_for_log_marker(const fs::path &dir, const std::string &marker,
                          std::chrono::milliseconds timeout =
                              std::chrono::milliseconds(8000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (read_hub_log(dir).find(marker) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/// Extract the port the broker bound to from a "Broker: listening on
/// tcp://<host>:<port>" log line.  Returns empty string on failure.
std::string extract_bound_endpoint(const std::string &log)
{
    static const std::regex re(R"(Broker: listening on (tcp://[^\s]+))");
    std::smatch m;
    if (std::regex_search(log, m, re)) return m[1].str();
    return {};
}

/// Build a producer.json that connects to the hub at @p hub_dir.
/// Mirrors `producer_init.cpp`'s template minus the schema (which we
/// pin explicitly here) and SHM (which the test doesn't exercise —
/// transport=zmq simplifies the no-SHM-perms-tweak path).
void write_producer_config(const fs::path &cfg_path,
                            const fs::path &hub_dir,
                            const std::string &channel)
{
    nlohmann::json j;
    j["producer"]["uid"]       = "prod.l4round.uid12345678";
    j["producer"]["name"]      = "L4RoundProducer";
    j["producer"]["log_level"] = "info";
    j["producer"]["auth"]["keyfile"] = "";  // role-side CURVE off

    j["out_hub_dir"]      = hub_dir.string();
    j["out_channel"]      = channel;
    j["loop_timing"]      = "fixed_rate";
    j["target_period_ms"] = 100;

    // Use ZMQ transport so no SHM block is required (SHM perms +
    // sysv key handling complicates L4 cross-subprocess teardown).
    j["out_transport"]      = "zmq";
    j["out_zmq_endpoint"]   = "tcp://127.0.0.1:0";
    j["out_zmq_bind"]       = true;
    j["out_zmq_buffer_depth"] = 8;
    // Note: "out_zmq_packing" removed 2026-04-20 — packing now lives
    // in the schema (out_slot_schema.packing).

    j["out_slot_schema"]["packing"] = "aligned";
    j["out_slot_schema"]["fields"]  = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });

    j["checksum"]             = "manual";
    j["stop_on_script_error"] = false;
    j["script"]["type"]       = "python";
    j["script"]["path"]       = ".";

    std::error_code ec;
    fs::create_directories(cfg_path.parent_path(), ec);
    std::ofstream f(cfg_path);
    f << j.dump(2);
}

/// Trivial Python script — defines on_init and on_produce that
/// returns False (don't publish); keeps the role alive without
/// generating noise.  on_stop too for clean shutdown.
void write_producer_script(const fs::path &script_dir)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "def on_init(api):\n"
         "    pass\n"
         "\n"
         "def on_produce(tx, msgs, api):\n"
         "    return False  # discard slot — keep loop minimal\n"
         "\n"
         "def on_stop(api):\n"
         "    pass\n";
}

/// Path to the staged plh_role binary (sibling of plh_hub_binary()).
std::string plh_role_binary()
{
    return (fs::path(::g_self_exe_path).parent_path()
            / ".." / "bin" / "plh_role").string();
}

} // namespace

// ─── Tests ─────────────────────────────────────────────────────────────────

/// THE F1 regression test:  with CURVE auth wired (--keygen produces
/// both vault AND hub.pubkey), a producer can connect to the hub and
/// register a channel end-to-end.  Pre-F1, hub.pubkey was never
/// written; the role's CURVE pin would be empty; the connection
/// would either fail or silently downgrade to anonymous depending on
/// the broker's auth state.
TEST_F(PlhHubCliTest, RoundTrip_PlhHubKeygenAndRunPlhRoleRegisters)
{
    // ── Hub side ────────────────────────────────────────────────────────────
    const fs::path hub_dir = tmp("rtrip_hub");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {hub_dir.string(), "--name", "L4RoundtripHub"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }

    // Configure for run-mode: ephemeral broker port, admin off, no
    // script.  CURVE auth stays ON (the F1 path).  --keygen will
    // populate vault + pubkey at the documented locations.
    {
        nlohmann::json j;
        {
            std::ifstream f(hub_dir / "hub.json");
            f >> j;
        }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["path"]             = "";
        // Leave hub.auth.keyfile = "vault/hub.vault" from the template.
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    // Run --keygen.  Pre-F1 this wrote only the vault file; post-F1
    // it ALSO writes <hub_dir>/hub.pubkey via
    // HubVault::publish_public_key called from
    // HubConfig::create_keypair.
    ::setenv("PYLABHUB_HUB_PASSWORD", "rtrip-test", /*overwrite=*/1);
    {
        WorkerProcess kg(plh_hub_binary(), "--config",
            {(hub_dir / "hub.json").string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << kg.get_stderr();
    }
    // Affirmative F1 check — both files present at HEP §7 paths.
    ASSERT_TRUE(fs::exists(hub_dir / "vault" / "hub.vault"))
        << "F1 regression: vault file not at HEP §7 path";
    ASSERT_TRUE(fs::exists(hub_dir / "hub.pubkey"))
        << "F1 regression: hub.pubkey not published by --keygen — role "
           "would have an empty CURVE pin and reject (or silently "
           "downgrade) the broker connection";

    // Spawn the hub run-mode process.
    WorkerProcess hub(plh_hub_binary(), hub_dir.string(), {});
    ASSERT_TRUE(wait_for_log_marker(hub_dir, "Broker: listening on"))
        << "hub never reached run-mode.  Log:\n" << read_hub_log(hub_dir);

    // Read the actual bound endpoint (kernel assigned port from
    // tcp://...:0) and PATCH hub.json so the role sees the real port.
    // The role reads `network.broker_endpoint` at config-load time.
    const std::string bound_ep = extract_bound_endpoint(read_hub_log(hub_dir));
    ASSERT_FALSE(bound_ep.empty())
        << "could not parse 'Broker: listening on' line from hub log";
    {
        nlohmann::json j;
        {
            std::ifstream f(hub_dir / "hub.json");
            f >> j;
        }
        j["network"]["broker_endpoint"] = bound_ep;
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    // ── Role side ───────────────────────────────────────────────────────────
    const fs::path prod_dir = tmp("rtrip_prod");
    write_producer_config(prod_dir / "producer.json", hub_dir,
                          "lab.l4.roundtrip");
    write_producer_script(prod_dir / "script" / "python");

    WorkerProcess role(plh_role_binary(), "--role",
        {"producer", prod_dir.string()});

    // The PATH-DISCRIMINATING success marker: hub-side broker logs
    // "Broker: registered channel '<name>'" only when REG_REQ
    // succeeded end-to-end (CURVE handshake passed, schema valid,
    // role registered into HubState).  Pre-F1 the CURVE handshake
    // would fail (empty pubkey on role side); the marker would
    // never appear; the watchdog times out cleanly.
    EXPECT_TRUE(wait_for_log_marker(hub_dir,
                                     "Broker: registered channel "
                                     "'lab.l4.roundtrip'",
                                     std::chrono::seconds(15)))
        << "role failed to register channel via broker.  This is the F1\n"
           "regression — without hub.pubkey published by --keygen the\n"
           "role's CURVE pin is empty.  Hub log:\n"
        << read_hub_log(hub_dir)
        << "\n--- Role stderr ---\n" << role.get_stderr();

    // ── Shutdown both ──────────────────────────────────────────────────────
    role.send_signal(SIGTERM);
    EXPECT_EQ(role.wait_for_exit(10), 0)
        << "plh_role did not exit cleanly on SIGTERM.  stderr:\n"
        << role.get_stderr();

    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0)
        << "plh_hub did not exit cleanly on SIGTERM.  stderr:\n"
        << hub.get_stderr();

    // ── Class-D gate: no [ERROR ] in either log/stderr ─────────────────────
    auto contains_error = [](const std::string &s) {
        return s.find("[ERROR ]") != std::string::npos;
    };
    const std::string hub_log = read_hub_log(hub_dir);
    EXPECT_FALSE(contains_error(hub_log))
        << "hub log contains [ERROR ] after clean SIGTERM:\n" << hub_log;
    EXPECT_FALSE(contains_error(role.get_stderr()))
        << "role stderr contains [ERROR ] after clean SIGTERM:\n"
        << role.get_stderr();

    ::unsetenv("PYLABHUB_HUB_PASSWORD");
}
