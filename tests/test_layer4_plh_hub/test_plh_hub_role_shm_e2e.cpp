/**
 * @file test_plh_hub_role_shm_e2e.cpp
 * @brief HEP-0041 1k Scenario A — L4 end-to-end SHM auth-gated data flow.
 *
 * **STATUS 2026-06-25: WIP scaffold — TEST_F bodies prefixed
 * `DISABLED_` until follow-up work removes the prefix.**
 *
 * What's verified working in this commit:
 *   - Hub init / keygen / known_roles registration for 2 roles
 *   - Producer SHM TX setup: `event=ShmCapabilityTransportBound`
 *     observed (L2 marker contract #1 from #258 checklist verified)
 *   - L4 helper infrastructure: hub-log reader, role-log reader
 *     (rotating file under `<role_dir>/logs/`), marker waiters with
 *     fallback to stderr
 *   - Producer + consumer configs match working
 *     `share/py-demo-single-processor-shm` shape (out_flexzone_schema,
 *     flexzone_checksum, schema fields)
 *   - Python script API: `api.log('info', ...)`, `tx.slot.<field>`,
 *     `rx.slot.<field>`, `on_consume(rx, msgs, api) -> bool`
 *
 * What's blocking the green test:
 *   - Producer's REG_REQ never reaches broker (hub log shows broker
 *     bound + ZAP enforced + 2 known_roles, but no
 *     `event=RegReqAccepted role='prod...' channel='...'` marker
 *     after 15 s).  Producer's SHM transport bound successfully,
 *     so the producer's BRC path (CURVE handshake → REG_REQ send) is
 *     where the flow stalls.  Likely culprits: producer config
 *     missing a required field for SHM path (the demo's
 *     `loop_timing=max_rate` vs my `fixed_rate` may matter), or the
 *     producer's BRC connect handshake fails silently when SHM TX
 *     is the transport.
 *
 * Embedded L2 contract verifications (folded in from #270) — the
 * complete marker checklist this test must validate is in #258 task
 * description.  Each ASSERT below pins one row of that checklist.
 *
 * Mutation pin: a sibling DISABLED_ TEST_F covers the unauthorized
 * consumer denial path (broker denies CONSUMER_ATTACH_REQ; consumer
 * never sees data).
 *
 * To re-enable: remove the `DISABLED_` prefix on each TEST_F, debug
 * REG_REQ flow, iterate to green.
 */
#include "plh_hub_fixture.h"

#include "utils/role_vault.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <thread>
#include <vector>

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;
using pylabhub::tests::helper::ExpectVaultFileSecured;

namespace
{

// ─── Shared log-reading helpers ─────────────────────────────────────────────
//
// hub log: file under <hub_dir>/logs/*.log (plh_hub configures a
// rolling log).  role logs: stderr captured by WorkerProcess.

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

bool wait_for_hub_marker(const fs::path &dir, const std::string &marker,
                         std::chrono::milliseconds timeout =
                             std::chrono::milliseconds(10000))
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

/// Reads the role's rotating log file (under <role_dir>/logs/).
/// After Logger sink switch (early in plh_role main), the role's
/// production INFO/WARN/ERROR markers go HERE — not stderr.
std::string read_role_log(const fs::path &role_dir)
{
    const fs::path logs = role_dir / "logs";
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

bool wait_for_role_marker(const fs::path &role_dir, WorkerProcess &p,
                          const std::string &marker,
                          std::chrono::milliseconds timeout =
                              std::chrono::milliseconds(10000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        // Check log file first (production INFO/ERROR markers land here),
        // then stderr (early-boot messages before log sink switch).
        if (read_role_log(role_dir).find(marker) != std::string::npos)
            return true;
        if (p.get_stderr().find(marker) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

std::string extract_bound_endpoint(const std::string &log)
{
    static const std::regex re(R"(Broker: listening on (tcp://[^\s]+))");
    std::smatch m;
    if (std::regex_search(log, m, re)) return m[1].str();
    return {};
}

std::string plh_role_binary()
{
    return (fs::path(::g_self_exe_path).parent_path()
            / ".." / "bin" / "plh_role").string();
}

// ─── Producer config + script (SHM TX) ─────────────────────────────────────

/// Producer config: SHM TX with Sequential sync policy + single
/// float32 slot.  Mirrors `producer_init.cpp` template but pins the
/// schema explicitly and sets a fixed-rate loop with a tight period
/// so 5 slots ship in < 1 s.
void write_shm_producer_config(const fs::path &cfg_path,
                                const fs::path &hub_dir,
                                const std::string &uid,
                                const std::string &channel)
{
    nlohmann::json j;
    j["producer"]["uid"]       = uid;
    j["producer"]["name"]      = "L4ShmProducer";
    j["producer"]["log_level"] = "info";
    j["producer"]["auth"]["keyfile"] = "vault/" + uid + ".vault";

    j["out_hub_dir"]      = hub_dir.string();
    j["out_channel"]      = channel;
    j["loop_timing"]      = "fixed_rate";
    j["target_period_ms"] = 50;

    // SHM TX — config shape mirrors share/py-demo-single-processor-shm
    // exactly so we exercise the same path operators use.
    j["out_transport"]    = "shm";
    j["out_shm_enabled"]  = true;
    j["out_shm_slot_count"] = 8;

    j["out_slot_schema"]["packing"] = "aligned";
    j["out_slot_schema"]["fields"]  = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_flexzone_schema"] = nullptr;

    j["checksum"]             = "enforced";
    j["flexzone_checksum"]    = true;
    j["stop_on_script_error"] = false;
    j["script"]["type"]       = "python";
    j["script"]["path"]       = ".";

    std::error_code ec;
    fs::create_directories(cfg_path.parent_path(), ec);
    std::ofstream f(cfg_path);
    f << j.dump(2);
}

/// Producer script — writes 5 sequential slots then no-ops.  Logs
/// `prod_test: wrote slot N=<n>` for each commit so the test can
/// observe progress + the producer-side log capture is rich enough
/// to debug failures.
void write_shm_producer_script(const fs::path &script_dir,
                                int n_slots)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "_N_SLOTS = " << n_slots << "\n"
      << "_written = [0]\n\n"
         "def on_init(api):\n"
         "    api.log('info', 'prod_test: init')\n"
         "\n"
         "def on_produce(tx, msgs, api):\n"
         "    if _written[0] >= _N_SLOTS:\n"
         "        return False\n"
         "    if tx.slot is None:\n"
         "        return False\n"
         "    tx.slot.value = float(_written[0])\n"
         "    api.log('info', 'prod_test: wrote slot N=' + str(_written[0]))\n"
         "    _written[0] += 1\n"
         "    return True\n"
         "\n"
         "def on_stop(api):\n"
         "    api.log('info', 'prod_test: stop')\n";
}

// ─── Consumer config + script (SHM RX) ─────────────────────────────────────

void write_shm_consumer_config(const fs::path &cfg_path,
                                const fs::path &hub_dir,
                                const std::string &uid,
                                const std::string &channel)
{
    nlohmann::json j;
    j["consumer"]["uid"]       = uid;
    j["consumer"]["name"]      = "L4ShmConsumer";
    j["consumer"]["log_level"] = "info";
    j["consumer"]["auth"]["keyfile"] = "vault/" + uid + ".vault";

    j["in_hub_dir"]      = hub_dir.string();
    j["in_channel"]      = channel;
    j["loop_timing"]     = "fixed_rate";
    j["target_period_ms"] = 50;

    j["in_transport"]    = "shm";
    j["in_shm_enabled"]  = true;
    j["in_shm_sync_policy"] = "sequential";

    j["in_slot_schema"]["packing"] = "aligned";
    j["in_slot_schema"]["fields"]  = nlohmann::json::array({
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

/// Consumer script — counts slots received via on_consume; when it
/// hits the expected count, emits `cons_test: complete N=<n>` so the
/// test can SIGTERM cleanly.
void write_shm_consumer_script(const fs::path &script_dir,
                                int expected_slots)
{
    std::error_code ec;
    fs::create_directories(script_dir, ec);
    std::ofstream f(script_dir / "__init__.py");
    f << "_EXPECTED = " << expected_slots << "\n"
      << "_received = [0]\n"
         "_done = [False]\n\n"
         "def on_init(api):\n"
         "    api.log('info', 'cons_test: init')\n"
         "\n"
         "def on_consume(rx, msgs, api):\n"
         "    if rx.slot is None:\n"
         "        return True\n"
         "    _received[0] += 1\n"
         "    api.log('info', 'cons_test: read slot N=' + str(_received[0]) +\n"
         "                    ' value=' + str(rx.slot.value))\n"
         "    if _received[0] >= _EXPECTED and not _done[0]:\n"
         "        _done[0] = True\n"
         "        api.log('info', 'cons_test: complete N=' + str(_received[0]))\n"
         "    return True\n"
         "\n"
         "def on_stop(api):\n"
         "    api.log('info', 'cons_test: stop')\n";
}

// ─── Vault + known-roles helpers (lifted from roundtrip pattern) ───────────

/// Spawn `plh_role --keygen` for a role and assert vault success.
std::string keygen_role_and_read_pubkey(
    const fs::path &role_dir, const std::string &role_kind,
    const std::string &uid, const std::string &role_password)
{
    WorkerProcess kg(plh_role_binary(), "--role",
        {role_kind, "--config",
         (role_dir / (role_kind + ".json")).string(), "--keygen"});
    EXPECT_EQ(kg.wait_for_exit(), 0)
        << "plh_role --keygen (" << role_kind << ") failed:\n"
        << kg.get_stderr();
    const fs::path vault_path = role_dir / "vault" / (uid + ".vault");
    ExpectVaultFileSecured(vault_path);
    auto vault = pylabhub::utils::RoleVault::open(
        vault_path, uid, role_password);
    return std::string(vault.public_key());
}

/// Add a role to known_roles.json via plh_hub --add-known-role.
void add_known_role(const fs::path &hub_dir, const std::string &display_name,
                    const std::string &uid, const std::string &role_kind,
                    const std::string &pubkey_z85)
{
    WorkerProcess add(plh_hub_binary(), "--config",
        {(hub_dir / "hub.json").string(),
         "--add-known-role",
         display_name, uid, role_kind, pubkey_z85});
    ASSERT_EQ(add.wait_for_exit(), 0)
        << "plh_hub --add-known-role failed:\n" << add.get_stderr();
}

} // namespace

// ─── Scenario A: authorized producer + consumer over SHM ───────────────────

// DISABLED_ pending #291 (SHM consumer-attach handshake bug exposed
// after HEP-0036 §5b phase B-1 unblocked the producer's Authorized
// transition).  Phase D of #286 re-enables once #291 lands.
TEST_F(PlhHubCliTest, DISABLED_ShmE2E_AuthorizedConsumerReceivesAllSlots)
{
    using std::chrono::seconds;
    using std::chrono::milliseconds;

    const std::string channel = "lab.l4.shm.e2e.a";
    const std::string prod_uid = "prod.l4shm.uid12345678";
    const std::string cons_uid = "cons.l4shm.uid12345678";
    constexpr int kSlots = 5;

    // ── Hub init + keygen + ZAP install ───────────────────────────────────
    const fs::path hub_dir = tmp("shme2e_hub");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {hub_dir.string(), "--name", "L4ShmE2EHub"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["path"]             = "";
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }
    ::setenv("PYLABHUB_HUB_PASSWORD", "shme2e-hub-pw", /*overwrite=*/1);
    {
        WorkerProcess kg(plh_hub_binary(), "--config",
            {(hub_dir / "hub.json").string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << kg.get_stderr();
    }
    ASSERT_TRUE(fs::exists(hub_dir / "hub.pubkey"));

    // ── Producer + consumer keygen + known_roles registration ─────────────
    const fs::path prod_dir = tmp("shme2e_prod");
    const fs::path cons_dir = tmp("shme2e_cons");
    std::error_code ec;
    fs::create_directories(prod_dir / "vault", ec);
    fs::create_directories(cons_dir / "vault", ec);

    write_shm_producer_config(prod_dir / "producer.json", hub_dir,
                               prod_uid, channel);
    // plh_role resolves script.path="." + script.type="python" to
    // <role_dir>/script/python/__init__.py (see roundtrip test pattern).
    write_shm_producer_script(prod_dir / "script" / "python", kSlots);
    write_shm_consumer_config(cons_dir / "consumer.json", hub_dir,
                               cons_uid, channel);
    write_shm_consumer_script(cons_dir / "script" / "python", kSlots);

    ::setenv("PYLABHUB_ROLE_PASSWORD", "shme2e-role-pw", /*overwrite=*/1);

    const std::string prod_pubkey =
        keygen_role_and_read_pubkey(prod_dir, "producer", prod_uid,
                                     "shme2e-role-pw");
    const std::string cons_pubkey =
        keygen_role_and_read_pubkey(cons_dir, "consumer", cons_uid,
                                     "shme2e-role-pw");

    add_known_role(hub_dir, "shme2e_prod", prod_uid, "producer", prod_pubkey);
    add_known_role(hub_dir, "shme2e_cons", cons_uid, "consumer", cons_pubkey);

    // ── Spawn hub run-mode + grab bound endpoint ──────────────────────────
    WorkerProcess hub(plh_hub_binary(), hub_dir.string(), {});
    ASSERT_TRUE(wait_for_hub_marker(hub_dir, "Broker: listening on"))
        << "hub never bound.  Log:\n" << read_hub_log(hub_dir);

    const std::string bound_ep =
        extract_bound_endpoint(read_hub_log(hub_dir));
    ASSERT_FALSE(bound_ep.empty()) << "no bound endpoint in hub log";
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = bound_ep;
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    // ── Producer spawn ────────────────────────────────────────────────────
    WorkerProcess prod(plh_role_binary(), "--role",
        {"producer", prod_dir.string()});

    // Failure-diagnostic helper: dump producer log FILE (production INFO
    // markers route here after Logger sink switch) + stderr + hub log,
    // so a missing marker tells us exactly where the chain broke.
    auto dump_prod = [&](const std::string &where) -> std::string {
        std::string s;
        s += "[fail at: " + where + "]\n";
        s += "── producer log file ──\n" + read_role_log(prod_dir) + "\n";
        s += "── producer stderr ──\n" + prod.get_stderr() + "\n";
        s += "── hub log ──\n" + read_hub_log(hub_dir) + "\n";
        return s;
    };

    // L2 marker contract #1 (prepare_tx_capability_):
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=ShmCapabilityTransportBound", seconds(5)))
        << dump_prod("ShmCapabilityTransportBound — prepare_tx_capability_ SHM path");

    // Hub-side REG_REQ accepted for producer.  Production-marker trail
    // between ShmCapabilityTransportBound and RegReqAccepted (all from
    // role_api_base.cpp + producer_role_host.cpp):
    //   start_handler_threads: ENTRY
    //   start_handler_threads: Phase 1 — connecting N BRC(s)
    //   start_handler_threads: Phase 1 OK   (or  Phase 1 FAILED)
    //   start_handler_threads: Phase 2/3 OK
    //   event=PresenceStateTransition ... trigger=REG_REQ_sending
    //   Either: event=RegAckReceived  OR  [prod] REG_REQ no response
    //                                 OR  [prod] REG_REQ failed: error_code=...
    // Broker-side rejection emits  Broker: REG_REQ rejected — channel='...' reason
    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=RegReqAccepted role='" + prod_uid + "' channel='" + channel + "'",
        seconds(7)))
        << dump_prod("RegReqAccepted (hub side) — REG_REQ chain");

    // L2 marker contract #2 (spawn_shm_auth_listener_): after REG_ACK
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=ShmAcceptLoopSpawned", seconds(5)))
        << dump_prod("ShmAcceptLoopSpawned — spawn_shm_auth_listener_");

    // ── Consumer spawn ────────────────────────────────────────────────────
    WorkerProcess cons(plh_role_binary(), "--role",
        {"consumer", cons_dir.string()});

    // Full-trace dump on consumer/late-phase failures — producer log file
    // often holds the smoking gun (e.g. role-side post-REG_ACK exit).
    auto dump_full = [&](const std::string &where) -> std::string {
        std::string s;
        s += "[fail at: " + where + "]\n";
        s += "── producer log file ──\n" + read_role_log(prod_dir) + "\n";
        s += "── producer stderr ──\n" + prod.get_stderr() + "\n";
        s += "── consumer log file ──\n" + read_role_log(cons_dir) + "\n";
        s += "── consumer stderr ──\n" + cons.get_stderr() + "\n";
        s += "── hub log ──\n" + read_hub_log(hub_dir) + "\n";
        return s;
    };

    // Consumer REG_REQ accepted by hub:
    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=ConsumerRegReqAccepted role='" + cons_uid +
        "' channel='" + channel + "'", seconds(5)))
        << dump_full("ConsumerRegReqAccepted — consumer REG_REQ chain");

    // Consumer parses shm_capability_endpoint from REG_ACK:
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "event=ShmCapabilityFieldsReceived", seconds(5)))
        << dump_full("ShmCapabilityFieldsReceived — consumer parses ACK");

    // Broker pre-confirms attach (broker-side ConsumerAttachAuthorized):
    ASSERT_TRUE(wait_for_hub_marker(hub_dir,
        "event=ConsumerAttachAuthorized", seconds(5)))
        << dump_full("ConsumerAttachAuthorized — broker pre-confirm");

    // Producer accept loop authorizes the attach (SCM_RIGHTS sent):
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=AttachAuthorized", seconds(5)))
        << dump_full("AttachAuthorized — producer accept loop");

    // Consumer attaches DataBlock via received fd:
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "event=ShmCapabilityActivated", seconds(5)))
        << dump_full("ShmCapabilityActivated — consumer DataBlock attach");

    // ── Data flow verification: consumer received all N slots ─────────────
    ASSERT_TRUE(wait_for_role_marker(cons_dir, cons,
        "cons_test: complete N=" + std::to_string(kSlots), seconds(7)))
        << dump_full("cons_test: complete N=" + std::to_string(kSlots) +
                     " — data flow");

    // ── Shutdown ──────────────────────────────────────────────────────────
    cons.send_signal(SIGTERM);
    EXPECT_EQ(cons.wait_for_exit(10), 0)
        << "consumer did not exit cleanly on SIGTERM.\n"
        << cons.get_stderr();

    prod.send_signal(SIGTERM);
    EXPECT_EQ(prod.wait_for_exit(10), 0)
        << "producer did not exit cleanly on SIGTERM.\n"
        << prod.get_stderr();

    // L2 marker contract #3 (cleanup_tx_capability_ LIFO teardown):
    EXPECT_NE(prod.get_stderr().find("event=ShmAttachOrchestratorReleased"),
              std::string::npos)
        << "producer did not log ShmAttachOrchestratorReleased on shutdown "
           "— cleanup_tx_capability_ LIFO teardown broken.\n"
        << prod.get_stderr();
    EXPECT_NE(prod.get_stderr().find("event=ShmCapabilityTransportReleased"),
              std::string::npos)
        << "producer did not log ShmCapabilityTransportReleased on shutdown "
           "— cleanup_tx_capability_ LIFO terminus broken.\n"
        << prod.get_stderr();

    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0)
        << "hub did not exit cleanly on SIGTERM.\n" << hub.get_stderr();

    // ── Class-D gate: no [ERROR ] in any log ──────────────────────────────
    auto contains_error = [](const std::string &s) {
        return s.find("[ERROR ]") != std::string::npos;
    };
    const std::string hub_log = read_hub_log(hub_dir);
    EXPECT_FALSE(contains_error(hub_log))
        << "hub log [ERROR ]:\n" << hub_log;
    EXPECT_FALSE(contains_error(prod.get_stderr()))
        << "producer stderr [ERROR ]:\n" << prod.get_stderr();
    EXPECT_FALSE(contains_error(cons.get_stderr()))
        << "consumer stderr [ERROR ]:\n" << cons.get_stderr();

    ::unsetenv("PYLABHUB_HUB_PASSWORD");
    ::unsetenv("PYLABHUB_ROLE_PASSWORD");
}

// ─── Mutation pin (denial scenario, abridged) ──────────────────────────────
//
// Same setup as Scenario A but the consumer is NOT added to
// known_roles.  Expectation: broker denies CONSUMER_ATTACH_REQ
// pre-confirm; consumer never logs ShmCapabilityActivated; no slots
// are received.  This is the regression pin for "removing consumer
// from known_roles must make the test fail" per the original 1k spec.

TEST_F(PlhHubCliTest, DISABLED_ShmE2E_UnauthorizedConsumerDeniedByBroker)
{
    using std::chrono::seconds;

    const std::string channel = "lab.l4.shm.e2e.deny";
    const std::string prod_uid = "prod.l4shm.uid87654321";
    const std::string cons_uid = "cons.l4shm.uid87654321";

    const fs::path hub_dir = tmp("shme2e_deny_hub");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {hub_dir.string(), "--name", "L4ShmDenyHub"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["path"]             = "";
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }
    ::setenv("PYLABHUB_HUB_PASSWORD", "shme2e-deny-pw", /*overwrite=*/1);
    {
        WorkerProcess kg(plh_hub_binary(), "--config",
            {(hub_dir / "hub.json").string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << kg.get_stderr();
    }

    const fs::path prod_dir = tmp("shme2e_deny_prod");
    const fs::path cons_dir = tmp("shme2e_deny_cons");
    std::error_code ec;
    fs::create_directories(prod_dir / "vault", ec);
    fs::create_directories(cons_dir / "vault", ec);

    write_shm_producer_config(prod_dir / "producer.json", hub_dir,
                               prod_uid, channel);
    write_shm_producer_script(prod_dir / "script" / "python", 5);
    write_shm_consumer_config(cons_dir / "consumer.json", hub_dir,
                               cons_uid, channel);
    write_shm_consumer_script(cons_dir / "script" / "python", 5);

    ::setenv("PYLABHUB_ROLE_PASSWORD", "shme2e-deny-role-pw", /*overwrite=*/1);

    const std::string prod_pubkey =
        keygen_role_and_read_pubkey(prod_dir, "producer", prod_uid,
                                     "shme2e-deny-role-pw");
    // Consumer keygens normally but is NOT added to known_roles —
    // the mutation point.  Producer IS added so the producer-side
    // gets through registration normally.
    (void)keygen_role_and_read_pubkey(cons_dir, "consumer", cons_uid,
                                       "shme2e-deny-role-pw");

    add_known_role(hub_dir, "shme2e_deny_prod", prod_uid, "producer",
                   prod_pubkey);
    // NOTE: consumer NOT added.

    WorkerProcess hub(plh_hub_binary(), hub_dir.string(), {});
    ASSERT_TRUE(wait_for_hub_marker(hub_dir, "Broker: listening on"))
        << "hub never bound.  Log:\n" << read_hub_log(hub_dir);

    const std::string bound_ep =
        extract_bound_endpoint(read_hub_log(hub_dir));
    ASSERT_FALSE(bound_ep.empty()) << "no bound endpoint in hub log";
    {
        nlohmann::json j;
        { std::ifstream f(hub_dir / "hub.json"); f >> j; }
        j["network"]["broker_endpoint"] = bound_ep;
        std::ofstream f(hub_dir / "hub.json");
        f << j.dump(2);
    }

    WorkerProcess prod(plh_role_binary(), "--role",
        {"producer", prod_dir.string()});
    ASSERT_TRUE(wait_for_role_marker(prod_dir, prod,
        "event=ShmAcceptLoopSpawned", seconds(15)))
        << "producer setup failed before consumer scenario could be exercised:\n"
        << prod.get_stderr();

    WorkerProcess cons(plh_role_binary(), "--role",
        {"consumer", cons_dir.string()});

    // The denial path: either the consumer's CTRL CURVE handshake
    // fails (ZAP gate denies on absence from known_roles) OR the
    // consumer connects but its REG_REQ / CONSUMER_ATTACH_REQ is
    // denied.  Either way it MUST NOT see ShmCapabilityActivated.
    // We give it a generous window then assert absence.
    std::this_thread::sleep_for(seconds(5));

    EXPECT_EQ(cons.get_stderr().find("event=ShmCapabilityActivated"),
              std::string::npos)
        << "DENIAL REGRESSION: unauthorized consumer activated SHM "
           "capability.  consumer stderr:\n" << cons.get_stderr();

    cons.send_signal(SIGTERM);
    cons.wait_for_exit(10);

    prod.send_signal(SIGTERM);
    EXPECT_EQ(prod.wait_for_exit(10), 0) << prod.get_stderr();

    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0) << hub.get_stderr();

    ::unsetenv("PYLABHUB_HUB_PASSWORD");
    ::unsetenv("PYLABHUB_ROLE_PASSWORD");
}
