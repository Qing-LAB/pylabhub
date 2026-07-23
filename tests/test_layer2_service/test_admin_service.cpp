/**
 * @file test_admin_service.cpp
 * @brief AdminService — the typed operator console (HEP-CORE-0033 §11).
 *
 * Pattern 1+ (BinaryLifecycleEnvironment).  Drives the real admin ROUTER of a
 * started `HubHost` over the typed `WireEnvelope` protocol via
 * `AdminWireClient` — the production path: a CURVE `DEALER` establishes a
 * session (ADMIN_HELLO_REQ → sealed session_id) then sends typed commands.
 *
 * Because `host.startup()` runs the REAL broker (its ZapRouter binds the
 * process ZAP handler on the shared context), a successful establish + ping
 * IS the §7.4/§11.1 regression guard: if the admin socket were routed to that
 * handler it would be rejected and the handshake/ping would time out
 * (HubHost_Console_EstablishAndPing).
 *
 * Contracts pinned:
 *   - Establishment: valid token → sealed session id; wrong token → error.
 *   - Session gate: a command without a valid session is rejected.
 *   - Query methods return a typed result; control methods a typed status.
 *   - Unknown msg_type → typed ADMIN_ERROR.
 */

#include "utils/admin_service.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "utils/wire_bodies.hpp"

#include "admin_wire_client.h"
#include "binary_lifecycle.h"
#include "curve_test_setup.h" // gen_curve_keypair, add_curve_identity
#include "log_capture_fixture.h"

#include "utils/config/hub_config.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/json_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/logger.hpp"
#include "utils/zmq_context.hpp"

#include <gtest/gtest.h>

#include <cppzmq/zmq.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace w = pylabhub::wire;
using nlohmann::json;
using pylabhub::admin::AdminService;
using pylabhub::config::HubAdminConfig;
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using pylabhub::tests::AdminWireClient;
using pylabhub::utils::HubDirectory;

PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule(),
                             pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
                             pylabhub::utils::FileLock::GetLifecycleModule(),
                             pylabhub::utils::JsonConfig::GetLifecycleModule(),
                             pylabhub::hub::GetZMQContextModule())

namespace
{

constexpr auto kTestToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/// Seed the hub broker identity (`kHubIdentityName`) once via the designed
/// facility so the admin ROUTER can arm its curve_server (§11.1) and the
/// console can pin it as `curve_serverkey`.  Returns the server Z85 pubkey.
const std::string &hub_server_pubkey()
{
    static const std::string pub = []
    {
        const auto kp = pylabhub::tests::gen_curve_keypair();
        pylabhub::tests::add_curve_identity(pylabhub::utils::security::kHubIdentityName, kp);
        return kp.public_z85;
    }();
    return pub;
}

fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l2_admin_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    return d;
}

template <typename Pred> bool poll_until(Pred pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

} // namespace

class AdminServiceTest : public ::testing::Test, public pylabhub::tests::LogCaptureFixture
{
  protected:
    void SetUp() override
    {
        (void)hub_server_pubkey(); // seed kHubIdentityName before any hub
        LogCaptureFixture::Install();
    }
    void TearDown() override
    {
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
        for (const auto &d : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(d, ec);
        }
    }

    /// A started HubHost with admin enabled; returns the admin ROUTER
    /// endpoint once bound.  Broker is up (ZAP handler active).
    std::string start_hub(const char *tag, std::size_t console_max_lines = 0)
    {
        const fs::path dir = unique_temp_dir(tag);
        paths_to_clean_.push_back(dir);
        fs::create_directories(dir);
        HubDirectory::init_directory(dir, "AdminTestHub");
        const fs::path hub_json = dir / "hub.json";
        json j;
        {
            std::ifstream f(hub_json);
            j = json::parse(f);
        }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"] = true;
        j["admin"]["endpoint"] = "tcp://127.0.0.1:0";
        if (console_max_lines != 0)
            j["admin"]["output_buffer"]["max_lines"] = console_max_lines;
        j["script"]["path"] = "";
        {
            std::ofstream f(hub_json);
            f << j.dump(2);
        }

        auto cfg = HubConfig::load_from_directory(dir.string());
        const_cast<HubAdminConfig &>(cfg.admin()).admin_token = kTestToken;

        host_.emplace(std::move(cfg));
        host_->startup();
        EXPECT_TRUE(host_->is_running());
        EXPECT_NE(host_->admin(), nullptr);
        EXPECT_TRUE(poll_until(
            [&] { return host_->admin() && !host_->admin()->bound_endpoint().empty(); }, 2000ms));
        return host_->admin() ? host_->admin()->bound_endpoint() : std::string{};
    }

    /// A CURVE console connected to @p endpoint with a fresh routing id.
    AdminWireClient make_console(const std::string &endpoint, const std::string &routing_id)
    {
        const auto ckp = pylabhub::tests::gen_curve_keypair();
        AdminWireClient::Config cfg{endpoint,       hub_server_pubkey(), ckp.public_z85,
                                    ckp.secret_z85, routing_id,          2000};
        return AdminWireClient(pylabhub::hub::get_zmq_context(), cfg);
    }

    void TearDownHub()
    {
        if (host_)
        {
            host_->shutdown();
            host_.reset();
        }
    }

    std::optional<HubHost> host_;
    std::vector<fs::path> paths_to_clean_;
};

// ─── Construction ───────────────────────────────────────────────────────────

TEST_F(AdminServiceTest, Console_EstablishAndPing_RealBroker)
{
    // The regression guard: broker up (ZAP handler live) + admin ROUTER; a
    // CURVE console must establish a session and ping.  If the admin socket
    // were ZAP-routed it would be rejected and this would time out.
    const std::string ep = start_hub("est_ping");
    ASSERT_FALSE(ep.empty());

    AdminWireClient console = make_console(ep, "op-console-1");
    ASSERT_TRUE(console.establish(kTestToken, "alice-laptop"));
    EXPECT_FALSE(console.session_id().empty());

    auto r = console.command(w::kAdminPingReq);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->is_error());
    EXPECT_EQ(r->msg_type, std::string(w::kAdminPingAck));
    EXPECT_EQ(r->body.value("status", std::string{}), "ok");

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_WrongToken_EstablishFails)
{
    const std::string ep = start_hub("bad_token");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");

    const std::string wrong(64, 'f');
    EXPECT_FALSE(console.establish(wrong, "mallory"));
    EXPECT_TRUE(console.session_id().empty());

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_CommandWithoutSession_Rejected)
{
    const std::string ep = start_hub("no_session");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");

    // No establish() — session_id is empty; a command must be rejected.
    auto r = console.command(w::kAdminPingReq);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_error());
    EXPECT_EQ(r->body.value("code", std::string{}), "unauthorized");

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_ListChannels_ReturnsResult)
{
    const std::string ep = start_hub("list_chan");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");
    ASSERT_TRUE(console.establish(kTestToken, "alice"));

    auto r = console.command(w::kAdminListChannelsReq);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->is_error());
    EXPECT_EQ(r->msg_type, std::string(w::kAdminListChannelsAck));
    EXPECT_TRUE(r->body.contains("result"));
    EXPECT_TRUE(r->body["result"].is_object());

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_QueryMetrics_ReturnsResult)
{
    const std::string ep = start_hub("metrics");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");
    ASSERT_TRUE(console.establish(kTestToken, "alice"));

    auto r = console.command(w::kAdminQueryMetricsReq, json{{"filter", json::object()}});
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->is_error());
    EXPECT_EQ(r->msg_type, std::string(w::kAdminQueryMetricsAck));
    EXPECT_TRUE(r->body["result"].is_object());

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_CloseUnknownChannel_NotFound)
{
    // `handle_close_channel` does a synchronous existence check and returns
    // `not_found` for an unknown channel, surfaced as a typed ADMIN_ERROR —
    // this pins the (preserved) handler behavior AND the handler-error →
    // ADMIN_ERROR mapping.
    //
    // Semantics resolved 2026-07-19 (CURVE-integration review): control
    // commands validate synchronously — an unknown channel is a typed
    // not_found, never a silent ok — and HEP-0033 §11.0.4 now states this.
    const std::string ep = start_hub("close_bogus");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");
    ASSERT_TRUE(console.establish(kTestToken, "alice"));

    auto r = console.command(w::kAdminCloseChannelReq, json{{"channel", "no.such.channel"}});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_error());
    EXPECT_EQ(r->msg_type, std::string(w::kAdminError));
    EXPECT_EQ(r->body.value("code", std::string{}), "not_found");

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_UnknownMsgType_Rejected)
{
    const std::string ep = start_hub("unknown");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");
    ASSERT_TRUE(console.establish(kTestToken, "alice"));

    auto r = console.command("ADMIN_BOGUS_REQ");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_error());
    EXPECT_EQ(r->body.value("code", std::string{}), "unknown_method");

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_ReplayedCommand_Rejected)
{
    // In-session replay guard (§11.0.5): a command frame replayed verbatim
    // (same client_nonce) within the window is rejected replay_or_skew.
    const std::string ep = start_hub("replay");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");
    ASSERT_TRUE(console.establish(kTestToken, "alice"));

    // Fixed nonce → sending the same body twice is a replay.
    const json body{{"session_id", console.session_id()},
                    {"client_nonce", "fixed-nonce-1"},
                    {"client_wall_ts", AdminWireClient::now_ms()}};

    auto r1 = console.request(w::kAdminPingReq, body);
    ASSERT_TRUE(r1.has_value());
    EXPECT_FALSE(r1->is_error()) << "first command must be accepted";

    auto r2 = console.request(w::kAdminPingReq, body); // same nonce = replay
    ASSERT_TRUE(r2.has_value());
    EXPECT_TRUE(r2->is_error());
    EXPECT_EQ(r2->body.value("code", std::string{}), "replay_or_skew");

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_AdminDisabled_NoAdmin)
{
    const fs::path dir = unique_temp_dir("disabled");
    paths_to_clean_.push_back(dir);
    fs::create_directories(dir);
    HubDirectory::init_directory(dir, "AdminTestHub");
    json j;
    {
        std::ifstream f(dir / "hub.json");
        j = json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"] = false;
    j["script"]["path"] = "";
    {
        std::ofstream f(dir / "hub.json");
        f << j.dump(2);
    }
    auto cfg = HubConfig::load_from_directory(dir.string());
    const_cast<HubAdminConfig &>(cfg.admin()).admin_token = kTestToken;

    host_.emplace(std::move(cfg));
    host_->startup();
    EXPECT_EQ(host_->admin(), nullptr) << "admin.enabled=false → no AdminService";
    TearDownHub();
}

// ── Output poll — RESPONSE_QUERY (HEP-CORE-0033 §11.0.4) ──────────────────────

TEST_F(AdminServiceTest, Console_ResponseQuery_EmptyBufferReturnsEmpty)
{
    // A poll of an empty buffer returns an explicit status:"empty" (never
    // silence, §11.0.4).  Exercises the read-gate (session + skew, no replay
    // nonce, §11.1) and the drain over the real CURVE wire.
    const std::string ep = start_hub("rq_empty");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");
    ASSERT_TRUE(console.establish(kTestToken, "alice"));

    auto r = console.command(w::kAdminResponseQueryReq);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->is_error());
    EXPECT_EQ(r->msg_type, std::string(w::kAdminResponseQueryAck));
    ASSERT_TRUE(r->body["result"].is_object());
    EXPECT_EQ(r->body["result"].value("status", std::string{}), "empty");
    EXPECT_TRUE(r->body["result"]["lines"].empty());
    EXPECT_EQ(r->body["result"].value("dropped_count", 999U), 0U);

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_ResponseQuery_ReturnsAppendedLinesThenClears)
{
    // The hub appends output lines — a command completion tagged with a
    // request_id (what the broker does, §11.0.4), and a script line with none
    // (what admin_console_print does) — and the console pulls them, matched by
    // request_id, oldest-first.  A second poll is empty (return-and-clear).
    const std::string ep = start_hub("rq_lines");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");
    ASSERT_TRUE(console.establish(kTestToken, "alice"));

    host_->append_console_line("req-42", json{{"event", "channel_closed"}, {"channel", "ch.x"}});
    host_->append_console_line("", json{{"message", "hello operator"}});

    auto r = console.command(w::kAdminResponseQueryReq);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->is_error());
    EXPECT_EQ(r->msg_type, std::string(w::kAdminResponseQueryAck));
    ASSERT_TRUE(r->body["result"].is_object());
    EXPECT_EQ(r->body["result"].value("status", std::string{}), "ok");
    const auto &lines = r->body["result"]["lines"];
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0].value("request_id", std::string{}), "req-42");
    EXPECT_EQ(lines[0]["content"].value("event", std::string{}), "channel_closed");
    EXPECT_EQ(lines[0]["content"].value("channel", std::string{}), "ch.x");
    EXPECT_TRUE(lines[0].contains("ts"));
    EXPECT_EQ(lines[1].value("request_id", std::string{}), "");
    EXPECT_EQ(lines[1]["content"].value("message", std::string{}), "hello operator");

    // Return-and-clear: the buffer is empty on the next poll.
    auto r2 = console.command(w::kAdminResponseQueryReq);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->body["result"].value("status", std::string{}), "empty");
    EXPECT_TRUE(r2->body["result"]["lines"].empty());

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_ResponseQuery_ConfigCapEnforced)
{
    // `admin.output_buffer.max_lines` from config must reach the buffer: with a
    // cap of 2, appending 3 lines spills the oldest to the log; the poll returns
    // only the newest 2 + dropped_count=1.  The spill WARN both proves the
    // config cap fired (not the default 1000) and is the expected log line.
    ExpectLogWarnMustFire("[admin_console] output line spilled");
    const std::string ep = start_hub("rq_cap", /*console_max_lines=*/2);
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");
    ASSERT_TRUE(console.establish(kTestToken, "alice"));

    host_->append_console_line("", json{{"message", "l1"}});
    host_->append_console_line("", json{{"message", "l2"}});
    host_->append_console_line("", json{{"message", "l3"}});

    auto r = console.command(w::kAdminResponseQueryReq);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->body["result"].is_object());
    EXPECT_EQ(r->body["result"].value("status", std::string{}), "ok");
    const auto &lines = r->body["result"]["lines"];
    ASSERT_EQ(lines.size(), 2u) << "config max_lines=2 not enforced; got " << lines.size();
    EXPECT_EQ(lines[0]["content"].value("message", std::string{}), "l2");
    EXPECT_EQ(lines[1]["content"].value("message", std::string{}), "l3");
    EXPECT_EQ(r->body["result"].value("dropped_count", 0U), 1U);

    TearDownHub();
}

TEST_F(AdminServiceTest, Console_ResponseQuery_NoSessionRejected)
{
    // The poll is session-gated (§11.1) even though it carries no replay nonce —
    // a missing/invalid session is rejected exactly like a command.
    const std::string ep = start_hub("rq_nosess");
    ASSERT_FALSE(ep.empty());
    AdminWireClient console = make_console(ep, "op-console-1");
    // No establish() — session_id is empty.
    auto r = console.command(w::kAdminResponseQueryReq);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->is_error());
    EXPECT_EQ(r->body.value("code", std::string{}), "unauthorized");

    TearDownHub();
}
