/**
 * @file test_admin_service.cpp
 * @brief AdminService Phase 6.2a skeleton — REP socket, token gate,
 *        localhost-bind invariant, round-trip ping, method dispatch
 *        stubs.  HEP-CORE-0033 §11.
 *
 * Pattern 2 — in-process LifecycleGuard via SetUpTestSuite (mirrors
 * test_hub_host.cpp).  Each test constructs a fresh AdminService (or
 * HubHost wrapping one), exercises one slice, and lets the destructor
 * clean up.
 *
 * Coverage:
 *   - Construction: token_required=false + non-loopback → throws.
 *   - Construction: token_required=true + empty token → throws.
 *   - Run/stop: spawned thread, bind endpoint visible, ping round-trip.
 *   - Token gate: valid / missing / wrong token.
 *   - Dispatch: ping ok, known stub → not_implemented, unknown_method.
 *   - dev_mode: token_required=false → ping accepted without token.
 *   - HubHost integration: admin enabled at startup → bound endpoint;
 *     ping via that endpoint works; HubHost::shutdown drains admin
 *     thread cleanly.
 *
 * NOTE: AdminService takes a HubHost reference even when no method
 * dispatcher actually uses it (Phase 6.2a is skeleton-only).  The
 * standalone tests construct a HubHost without calling startup() and
 * pass it as the backref — sufficient for the skeleton, will be
 * meaningful when 6.2b/c land.
 */

#include "utils/admin_service.hpp"

#include "log_capture_fixture.h"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/zmq_context.hpp"

#include <gtest/gtest.h>

#include <cppzmq/zmq.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using pylabhub::admin::AdminService;
using pylabhub::config::HubAdminConfig;
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using pylabhub::utils::FileLock;
using pylabhub::utils::HubDirectory;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::Logger;
using pylabhub::utils::MakeModDefList;
using nlohmann::json;

namespace
{

constexpr auto kTestToken =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l2_admin_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    return d;
}

/// Initialize a hub directory and patch its hub.json for L2 in-process
/// testing: ephemeral broker port, no CURVE auth, admin enabled with
/// an ephemeral admin port + token gate on by default.  Caller may
/// further mutate the JSON before re-reading.
nlohmann::json read_hub_json_for_test(const fs::path &dir,
                                       const std::string &name)
{
    fs::create_directories(dir);
    HubDirectory::init_directory(dir, name);
    const fs::path hub_json = dir / "hub.json";
    nlohmann::json j;
    {
        std::ifstream f(hub_json);
        EXPECT_TRUE(f.is_open()) << "test could not open " << hub_json;
        j = nlohmann::json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0"; // ephemeral
    j["admin"]["endpoint"]          = "tcp://127.0.0.1:0"; // ephemeral
    return j;
}

void write_hub_json(const fs::path &dir, const nlohmann::json &j)
{
    std::ofstream f(dir / "hub.json");
    f << j.dump(2);
}

/// Block until `pred()` returns true or `timeout` elapses.  Used to
/// wait for the admin worker to bind the REP socket and populate
/// `bound_endpoint()`.
template <typename Pred>
bool poll_until(Pred pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

/// Send a JSON request to a REP endpoint and return the parsed reply.
/// Caller-managed timeout via socket option; throws on timeout.
json req_reply(const std::string &endpoint, const json &request,
                std::chrono::milliseconds timeout = 2000ms)
{
    zmq::socket_t sock(pylabhub::hub::get_zmq_context(),
                       zmq::socket_type::req);
    sock.set(zmq::sockopt::linger, 0);
    sock.set(zmq::sockopt::rcvtimeo,
             static_cast<int>(timeout.count()));
    sock.set(zmq::sockopt::sndtimeo,
             static_cast<int>(timeout.count()));
    sock.connect(endpoint);

    const std::string req_str = request.dump();
    sock.send(zmq::buffer(req_str), zmq::send_flags::none);

    zmq::message_t reply;
    auto recv_result = sock.recv(reply, zmq::recv_flags::none);
    if (!recv_result.has_value())
        throw std::runtime_error("admin REP timed out");

    return json::parse(std::string_view(
        static_cast<const char *>(reply.data()), reply.size()));
}

} // namespace

// ─── Test fixture ──────────────────────────────────────────────────────────

class AdminServiceTest : public ::testing::Test,
                          public pylabhub::tests::LogCaptureFixture
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(MakeModDefList(
            Logger::GetLifecycleModule(),
            FileLock::GetLifecycleModule(),
            JsonConfig::GetLifecycleModule(),
            pylabhub::crypto::GetLifecycleModule(),
            pylabhub::hub::GetZMQContextModule()),
            std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override
    {
        // Capture logger output for the duration of the test.  Happy
        // paths must not emit WARN/ERROR; tests that drive an error
        // path declare the expected warning via ExpectLogWarn(...).
        LogCaptureFixture::Install();
    }

    void TearDown() override
    {
        // Verify no unexpected log entries before reverting capture
        // and cleaning temp dirs.
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    /// Returns a HubConfig + dir; admin is enabled with ephemeral port,
    /// token gate ON, admin_token = kTestToken.
    std::pair<HubConfig, fs::path> make_config(const char *tag,
        bool token_required = true, bool dev_mode = false)
    {
        const fs::path dir = unique_temp_dir(tag);
        paths_to_clean_.push_back(dir);
        auto j = read_hub_json_for_test(dir, "AdminTestHub");
        j["admin"]["enabled"]        = true;
        j["admin"]["token_required"] = token_required;
        j["admin"]["dev_mode"]       = dev_mode;
        write_hub_json(dir, j);

        auto cfg = HubConfig::load_from_directory(dir.string());
        // Bypass vault unlock: stamp the runtime-only admin_token field
        // directly.  HubConfig::load_keypair() (the production path)
        // reads it from `HubVault::admin_token()`; the test path skips
        // vault setup entirely.  Token gate behavior under test does
        // NOT depend on vault provenance.
        const_cast<HubAdminConfig &>(cfg.admin()).admin_token =
            token_required ? kTestToken : "";
        return {std::move(cfg), dir};
    }

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
    std::vector<fs::path>                  paths_to_clean_;
};

std::unique_ptr<LifecycleGuard> AdminServiceTest::s_lifecycle_;

// ─── Construction-time invariants ──────────────────────────────────────────

TEST_F(AdminServiceTest, Construct_TokenOff_NonLoopbackEndpoint_Throws)
{
    // §11.3 invariant: token_required==false demands a loopback bind.
    // Both this test and Construct_TokenOn_EmptyToken_Throws below use
    // the same exception type (std::invalid_argument); pin the message
    // substring so a regression that throws the OTHER ctor check
    // doesn't slip through this test.
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://0.0.0.0:5600";
    acfg.token_required = false;
    acfg.dev_mode       = true;

    auto [cfg, dir] = make_config("nonloopback");
    HubHost host(std::move(cfg)); // not started — backref only

    bool threw = false;
    std::string msg;
    try
    {
        AdminService(pylabhub::hub::get_zmq_context(),
                     acfg, /*token=*/"", host);
    }
    catch (const std::invalid_argument &e) { threw = true; msg = e.what(); }
    EXPECT_TRUE(threw);
    EXPECT_NE(msg.find("loopback"), std::string::npos)
        << "wrong invalid_argument path; what(): " << msg;
}

TEST_F(AdminServiceTest, Construct_TokenOn_EmptyToken_Throws)
{
    // §11.3 invariant: token gate ON requires a non-empty token,
    // otherwise every request silently authenticates.  Pin the
    // message substring (see comment on the loopback test above).
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("emptytoken");
    HubHost host(std::move(cfg));

    bool threw = false;
    std::string msg;
    try
    {
        AdminService(pylabhub::hub::get_zmq_context(),
                     acfg, /*token=*/"", host);
    }
    catch (const std::invalid_argument &e) { threw = true; msg = e.what(); }
    EXPECT_TRUE(threw);
    EXPECT_NE(msg.find("admin_token is empty"), std::string::npos)
        << "wrong invalid_argument path; what(): " << msg;
}

// ─── Standalone run loop + ping ────────────────────────────────────────────

TEST_F(AdminServiceTest, Run_PingRoundTrip_TokenGate)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("ping");
    HubHost host(std::move(cfg));

    AdminService svc(pylabhub::hub::get_zmq_context(),
                     acfg, kTestToken, host);

    std::thread worker([&] { svc.run(); });

    ASSERT_TRUE(poll_until(
        [&] { return !svc.bound_endpoint().empty(); }, 2000ms))
        << "AdminService did not publish bound endpoint within 2s";

    // ── Valid token → ok, echo arrives back ────────────────────────────────
    const json req = {
        {"method", "ping"},
        {"token",  kTestToken},
        {"params", {{"hello", "world"}}},
    };
    const json reply = req_reply(svc.bound_endpoint(), req);
    EXPECT_EQ(reply.value("status", ""), "ok");
    ASSERT_TRUE(reply.contains("result"));
    EXPECT_EQ(reply["result"].value("pong", false), true);
    EXPECT_EQ(reply["result"]["echo"]["hello"], "world");

    svc.stop();
    worker.join();
}

// ─── Token gate — wrong / missing token paths ──────────────────────────────

TEST_F(AdminServiceTest, TokenGate_Missing_Returns_Unauthorized)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("token_missing");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(),
                     acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until(
        [&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
                                  {{"method", "ping"}});
    EXPECT_EQ(reply.value("status", ""), "error");
    EXPECT_EQ(reply["error"]["code"], "unauthorized");

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, TokenGate_Wrong_Returns_Unauthorized)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("token_wrong");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(),
                     acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until(
        [&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    // Same-length wrong token — exercises the constant-time-equal
    // path, not the early-exit length check.
    const std::string wrong(64, 'f');
    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "ping"}, {"token", wrong}});
    EXPECT_EQ(reply.value("status", ""), "error");
    EXPECT_EQ(reply["error"]["code"], "unauthorized");

    svc.stop();
    worker.join();
}

// ─── Method dispatch — stubs and unknown ───────────────────────────────────

TEST_F(AdminServiceTest, Dispatch_DeferredMethod_Returns_NotImplemented)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("stub");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(),
                     acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until(
        [&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    // `reload_config` is a known §11.2 method but explicitly deferred
    // (HEP-0033 §16 #9 — runtime-tunable whitelist not yet enumerated).
    // It must surface as `not_implemented`, not as `unknown_method`,
    // so a client typo is distinguishable from a staged feature.
    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "reload_config"}, {"token", kTestToken}});
    ASSERT_EQ(reply.value("status", ""), "error")
        << "reload_config is deferred; expected status=error, got: " << reply.dump();
    EXPECT_EQ(reply["error"]["code"], "not_implemented");

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Dispatch_UnknownMethod_Returns_UnknownMethod)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("unknown");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(),
                     acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until(
        [&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "totally_made_up_method"}, {"token", kTestToken}});
    EXPECT_EQ(reply.value("status", ""), "error");
    EXPECT_EQ(reply["error"]["code"], "unknown_method");

    svc.stop();
    worker.join();
}

// ─── §11.2 query methods (Phase 6.2b) ──────────────────────────────────────
//
// These tests exercise the read-through dispatch paths that touch
// `host.state()` (HubState) without needing the broker thread to be
// running.  HubState is a value member of HubHost, so its accessors
// are safe before `host.startup()`; the result snapshots are empty
// (zero channels / roles / bands / peers in a freshly-constructed
// host).  Each test pins:
//   - status == "ok" and result is the right shape (object/empty)
//   - error.code on the negative paths (path-discrimination per
//     audit Class A — matches HEP-0033 §11.5 catalog).

TEST_F(AdminServiceTest, Query_ListChannels_Empty_ReturnsOkObject)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("q_list_chan");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "list_channels"}, {"token", kTestToken}});
    ASSERT_EQ(reply.value("status", ""), "ok") << reply.dump();
    ASSERT_TRUE(reply.contains("result"));
    EXPECT_TRUE(reply["result"].is_object());
    EXPECT_TRUE(reply["result"].empty()) << "no channels expected pre-startup";

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Query_GetChannel_MissingParams_Returns_InvalidParams)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("q_get_chan_bad");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    // No params block → invalid_params.
    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "get_channel"}, {"token", kTestToken}});
    ASSERT_EQ(reply.value("status", ""), "error") << reply.dump();
    EXPECT_EQ(reply["error"]["code"], "invalid_params");
    // Message must mention the missing field so the operator can fix it.
    EXPECT_NE(reply["error"]["message"].get<std::string>().find("channel"),
              std::string::npos);

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Query_GetChannel_Unknown_Returns_NotFound)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("q_get_chan_404");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "get_channel"},
         {"token",  kTestToken},
         {"params", {{"channel", "no.such.channel"}}}});
    ASSERT_EQ(reply.value("status", ""), "error") << reply.dump();
    EXPECT_EQ(reply["error"]["code"], "not_found");

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Query_ListRoles_Empty_ReturnsOkObject)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("q_list_roles");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "list_roles"}, {"token", kTestToken}});
    ASSERT_EQ(reply.value("status", ""), "ok") << reply.dump();
    EXPECT_TRUE(reply["result"].is_object());
    EXPECT_TRUE(reply["result"].empty());

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Query_GetRole_Unknown_Returns_NotFound)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("q_get_role_404");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "get_role"},
         {"token",  kTestToken},
         {"params", {{"uid", "prod.nope.uid00000000"}}}});
    ASSERT_EQ(reply.value("status", ""), "error") << reply.dump();
    EXPECT_EQ(reply["error"]["code"], "not_found");

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Query_ListBands_Empty_ReturnsOkObject)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("q_list_bands");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "list_bands"}, {"token", kTestToken}});
    ASSERT_EQ(reply.value("status", ""), "ok") << reply.dump();
    EXPECT_TRUE(reply["result"].is_object());
    EXPECT_TRUE(reply["result"].empty());

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Query_ListPeers_Empty_ReturnsOkObject)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("q_list_peers");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "list_peers"}, {"token", kTestToken}});
    ASSERT_EQ(reply.value("status", ""), "ok") << reply.dump();
    EXPECT_TRUE(reply["result"].is_object());
    EXPECT_TRUE(reply["result"].empty());

    svc.stop();
    worker.join();
}



TEST_F(AdminServiceTest, DevMode_TokenSkipped_PingAccepted)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = false;
    acfg.dev_mode       = true;

    auto [cfg, dir] = make_config("dev", /*token_required=*/false,
                                  /*dev_mode=*/true);
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(),
                     acfg, /*token=*/"", host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until(
        [&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    // No `token` field at all — accepted because token_required=false.
    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "ping"}});
    EXPECT_EQ(reply.value("status", ""), "ok");
    EXPECT_EQ(reply["result"].value("pong", false), true);

    svc.stop();
    worker.join();
}

// ─── §11.2 control methods (Phase 6.2c) ────────────────────────────────────
//
// Negative paths only at the standalone-AdminService level (no broker
// running).  Positive paths require a startup-ed HubHost and are
// covered in the HubHost_Admin* integration block below.

TEST_F(AdminServiceTest, Control_CloseChannel_MissingParams_Returns_InvalidParams)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("c_close_bad");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "close_channel"}, {"token", kTestToken}});
    ASSERT_EQ(reply.value("status", ""), "error") << reply.dump();
    EXPECT_EQ(reply["error"]["code"], "invalid_params");
    EXPECT_NE(reply["error"]["message"].get<std::string>().find("channel"),
              std::string::npos);

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Control_CloseChannel_Unknown_Returns_NotFound)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("c_close_404");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "close_channel"},
         {"token",  kTestToken},
         {"params", {{"channel", "no.such.channel"}}}});
    ASSERT_EQ(reply.value("status", ""), "error") << reply.dump();
    EXPECT_EQ(reply["error"]["code"], "not_found");

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Control_BroadcastChannel_MissingMessage_Returns_InvalidParams)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("c_bcast_bad");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    // channel present but message missing → invalid_params (not silent
    // accept of an empty message).
    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "broadcast_channel"},
         {"token",  kTestToken},
         {"params", {{"channel", "lab.x"}}}});
    ASSERT_EQ(reply.value("status", ""), "error") << reply.dump();
    EXPECT_EQ(reply["error"]["code"], "invalid_params");
    EXPECT_NE(reply["error"]["message"].get<std::string>().find("message"),
              std::string::npos);

    svc.stop();
    worker.join();
}

TEST_F(AdminServiceTest, Control_BroadcastChannel_Unknown_Returns_NotFound)
{
    HubAdminConfig acfg;
    acfg.endpoint       = "tcp://127.0.0.1:0";
    acfg.token_required = true;

    auto [cfg, dir] = make_config("c_bcast_404");
    HubHost host(std::move(cfg));
    AdminService svc(pylabhub::hub::get_zmq_context(), acfg, kTestToken, host);
    std::thread worker([&] { svc.run(); });
    ASSERT_TRUE(poll_until([&] { return !svc.bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(svc.bound_endpoint(),
        {{"method", "broadcast_channel"},
         {"token",  kTestToken},
         {"params", {{"channel", "no.such.channel"}, {"message", "go"}}}});
    ASSERT_EQ(reply.value("status", ""), "error") << reply.dump();
    EXPECT_EQ(reply["error"]["code"], "not_found");

    svc.stop();
    worker.join();
}

// ─── HubHost integration — admin spawned + drained ─────────────────────────

TEST_F(AdminServiceTest, HubHost_AdminEnabled_RoundTripWorks)
{
    // The teardown-race that previously surfaced here (LifecycleGuard
    // logging a WARN for the documented-normal validator-fail on
    // owner-managed modules) was fixed in production: validator-fail
    // for an UNLOADING module is now logged at DEBUG and the module
    // entry is fully cleaned from the graph so subsequent
    // re-registration with the same name succeeds.  No
    // `ExpectLogWarn` papering needed.
    auto [cfg, dir] = make_config("hubhost");
    HubHost host(std::move(cfg));

    host.startup();
    EXPECT_TRUE(host.is_running());

    // Admin pointer is non-null and bound endpoint becomes available
    // shortly after the admin thread starts running.
    ASSERT_NE(host.admin(), nullptr);
    ASSERT_TRUE(poll_until(
        [&] { return !host.admin()->bound_endpoint().empty(); }, 2000ms));

    // End-to-end ping over the host-driven admin endpoint.  Verify the
    // full result envelope, not just status — a regression that
    // returned `{"status":"ok", "result":{}}` would slip past a
    // status-only check (verified via mutation sweep 2026-05-01).
    const json reply = req_reply(host.admin()->bound_endpoint(),
        {{"method", "ping"}, {"token", kTestToken},
         {"params", {{"trip", 42}}}});
    EXPECT_EQ(reply.value("status", ""), "ok");
    ASSERT_TRUE(reply.contains("result"));
    EXPECT_EQ(reply["result"].value("pong", false), true);
    EXPECT_EQ(reply["result"]["echo"].value("trip", 0), 42);

    host.shutdown();
    EXPECT_FALSE(host.is_running());
    // After shutdown(), the admin pointer is still owned by HubHost
    // (cleared at next startup or at host destruction); but the
    // service has been stopped — its bound endpoint is closed.
}

TEST_F(AdminServiceTest, HubHost_AdminEnabled_QueryMetricsRoundTrip)
{
    // query_metrics calls `host.broker().query_metrics_json_str()`,
    // which is UB before HubHost::startup().  This test runs the
    // full lifecycle so the broker thread is up.
    auto [cfg, dir] = make_config("metrics");
    HubHost host(std::move(cfg));

    host.startup();
    ASSERT_TRUE(host.is_running());
    ASSERT_NE(host.admin(), nullptr);
    ASSERT_TRUE(poll_until(
        [&] { return !host.admin()->bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(host.admin()->bound_endpoint(),
        {{"method", "query_metrics"}, {"token", kTestToken}});
    ASSERT_EQ(reply.value("status", ""), "ok") << reply.dump();
    // The broker's metrics document is wrapped in our envelope's
    // "result" field; structural pin: status==success in the
    // metrics body, plus the channels/roles/bands/peers categories
    // present (even if empty).  Without the inner status check, an
    // envelope-only "ok" could slip past on a regression that returned
    // an empty/garbled metrics document.
    ASSERT_TRUE(reply["result"].is_object());
    EXPECT_EQ(reply["result"].value("status", ""), "success");
    EXPECT_TRUE(reply["result"].contains("channels"));
    EXPECT_TRUE(reply["result"].contains("roles"));
    EXPECT_TRUE(reply["result"].contains("queried_at"));

    host.shutdown();
}

TEST_F(AdminServiceTest, HubHost_AdminEnabled_RequestShutdown_StopsHost)
{
    // Positive control-method path.  request_shutdown must:
    //   - return ok with shutdown_requested=true
    //   - flip the host's shutdown atomic so run_main_loop returns
    //   - leave the host in a state that accepts host.shutdown()
    auto [cfg, dir] = make_config("rsd");
    HubHost host(std::move(cfg));

    host.startup();
    ASSERT_TRUE(host.is_running());
    ASSERT_NE(host.admin(), nullptr);
    ASSERT_TRUE(poll_until(
        [&] { return !host.admin()->bound_endpoint().empty(); }, 2000ms));

    const json reply = req_reply(host.admin()->bound_endpoint(),
        {{"method", "request_shutdown"}, {"token", kTestToken}});
    ASSERT_EQ(reply.value("status", ""), "ok") << reply.dump();
    EXPECT_EQ(reply["result"].value("shutdown_requested", false), true);

    // The post-RPC host.shutdown() must succeed cleanly (no double-
    // request issue, no resource leak).  request_shutdown is the
    // soft-stop signal that run_main_loop would observe; this test
    // doesn't enter run_main_loop, so we verify the synchronous path
    // by calling shutdown() and asserting clean state-machine
    // transition.
    host.shutdown();
    EXPECT_FALSE(host.is_running());
}

TEST_F(AdminServiceTest, HubHost_AdminDisabled_NoAdminConstructed)
{
    // See HubHost_AdminEnabled_RoundTripWorks — the production fix
    // for the teardown-race makes ExpectLogWarn unnecessary.
    auto [cfg, dir] = make_config("disabled");
    // Disable admin in-place.
    const_cast<HubAdminConfig &>(cfg.admin()).enabled = false;
    HubHost host(std::move(cfg));

    host.startup();
    EXPECT_EQ(host.admin(), nullptr) << "admin.enabled=false → no AdminService";
    host.shutdown();
}
