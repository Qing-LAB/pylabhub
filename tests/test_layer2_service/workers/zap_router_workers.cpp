/**
 * @file zap_router_workers.cpp
 * @brief Worker bodies for the ZapRouter L2 test suite (PeerAdmission Phase C).
 *
 * Pattern 3 — each worker runs in a fresh subprocess with its own
 * LifecycleGuard (Logger + FileLock + JsonConfig + ZMQContext per
 * the ZapRouter dynamic-module dependency chain).
 *
 * Each handshake test uses a `ZapPumpThread` RAII helper to pump the
 * inproc ZAP socket while the test thread drives CURVE handshakes
 * via a real `tcp://127.0.0.1:0` socket pair.  In production
 * binaries the pump is in the broker's / role's existing event loop
 * (Phase D wiring) — never `ZapPumpThread`.  The L2 test using the
 * RAII helper is functionally equivalent to "your main loop forgot
 * to pump" being immediately observable as a test hang.
 */
#include "utils/file_lock.hpp"
#include "utils/json_config.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/security/peer_admission.hpp"
#include "utils/security/zap_router.hpp"
#include "utils/zmq_context.hpp"

#include "cppzmq/zmq.hpp"
#include <zmq.h>

#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;
using pylabhub::utils::security::PeerAdmission;
using pylabhub::utils::security::PeerAllowlist;
using pylabhub::utils::security::PeerIdentity;
using pylabhub::utils::security::ZapDomainHandle;
using pylabhub::utils::security::ZapPumpThread;
using pylabhub::utils::security::ZapRouter;

namespace
{

constexpr std::size_t kPubkeyZ85Chars = 40;

class InMemoryAdmission final : public PeerAdmission
{
public:
    bool set_peer_allowlist(PeerAllowlist al) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        al_ = std::move(al);
        return true;
    }
    std::optional<PeerAllowlist>
    peer_allowlist_snapshot() const override
    {
        std::lock_guard<std::mutex> lk(mu_);
        return al_;
    }
    bool is_peer_allowed(const PeerIdentity &p) const override
    {
        std::lock_guard<std::mutex> lk(mu_);
        return al_.has_value() && al_->contains(p);
    }
    bool admission_is_enforced() const noexcept override { return true; }

private:
    mutable std::mutex          mu_;
    std::optional<PeerAllowlist> al_;
};

std::pair<std::string, std::string> make_keypair()
{
    std::array<char, kPubkeyZ85Chars + 1> pub{};
    std::array<char, kPubkeyZ85Chars + 1> sec{};
    if (::zmq_curve_keypair(pub.data(), sec.data()) != 0)
        throw std::runtime_error("zmq_curve_keypair failed");
    return {std::string(pub.data(), kPubkeyZ85Chars),
            std::string(sec.data(), kPubkeyZ85Chars)};
}

zmq::socket_t bind_pull_server(const std::string &server_pub,
                                const std::string &server_sec,
                                const std::string &zap_domain)
{
    zmq::socket_t pull(pylabhub::hub::get_zmq_context(),
                       zmq::socket_type::pull);
    pull.set(zmq::sockopt::linger, 0);
    pull.set(zmq::sockopt::curve_server, 1);
    pull.set(zmq::sockopt::curve_publickey, server_pub);
    pull.set(zmq::sockopt::curve_secretkey, server_sec);
    pull.set(zmq::sockopt::zap_domain, zap_domain);
    pull.bind("tcp://127.0.0.1:0");
    return pull;
}

/// Drive one CURVE handshake by spinning up a fresh PUSH socket;
/// send one byte; observe whether the PULL side receives it within
/// @p timeout.  Returns true on delivery, false on timeout (which
/// means ZAP denied OR the network round-trip didn't complete).
bool handshake_and_deliver(zmq::socket_t      &pull,
                            const std::string &server_pub,
                            const std::string &client_pub,
                            const std::string &client_sec,
                            std::chrono::milliseconds timeout)
{
    zmq::socket_t push(pylabhub::hub::get_zmq_context(),
                       zmq::socket_type::push);
    push.set(zmq::sockopt::linger, 0);
    push.set(zmq::sockopt::curve_serverkey, server_pub);
    push.set(zmq::sockopt::curve_publickey, client_pub);
    push.set(zmq::sockopt::curve_secretkey, client_sec);
    const std::string endpoint =
        pull.get(zmq::sockopt::last_endpoint);
    push.connect(endpoint);

    zmq::message_t out(1);
    std::memcpy(out.data(), "x", 1);
    (void) push.send(out, zmq::send_flags::dontwait);

    zmq::message_t in;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        pull.set(zmq::sockopt::rcvtimeo, 50);
        try
        {
            auto rr = pull.recv(in, zmq::recv_flags::none);
            if (rr.has_value() && in.size() == 1)
                return true;
        }
        catch (const zmq::error_t &)
        {
            break;
        }
    }
    return false;
}

// ── Scenarios ──────────────────────────────────────────────────────────────

int handshake_accept_deny_cycle(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            const auto [server_pub,  server_sec]  = make_keypair();
            const auto [allowed_pub, allowed_sec] = make_keypair();
            const auto [denied_pub,  denied_sec]  = make_keypair();

            InMemoryAdmission admission;
            auto pull = bind_pull_server(server_pub, server_sec,
                                          "test.zap.handshake.suite1");
            auto handle = ZapRouter::instance().register_domain(
                "test.zap.handshake.suite1", &admission);
            ASSERT_TRUE(handle.is_active());
            EXPECT_EQ(ZapRouter::instance()
                          .registered_domain_count_for_test(), 1u);

            // Spin up the pump.  RAII: joins at scope exit, before
            // ASSERT_TRUE returns / before destructors of `pull` /
            // `handle` run.
            ZapPumpThread pump;

            // Phase 1: no allowlist → deny everyone (secure default).
            EXPECT_FALSE(handshake_and_deliver(
                pull, server_pub, allowed_pub, allowed_sec,
                std::chrono::milliseconds(300)));

            // Phase 2: allow `allowed_pub` → handshake succeeds.
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{"curve", allowed_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al));
            EXPECT_TRUE(handshake_and_deliver(
                pull, server_pub, allowed_pub, allowed_sec,
                std::chrono::milliseconds(1000)));

            // Phase 3: different peer with valid CURVE but NOT in
            // allowlist → deny.
            EXPECT_FALSE(handshake_and_deliver(
                pull, server_pub, denied_pub, denied_sec,
                std::chrono::milliseconds(300)));

            // Phase 4: mid-flight allowlist swap → new peer
            // allowed.
            PeerAllowlist al2;
            al2.peers.insert(PeerIdentity{"curve", denied_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al2));
            EXPECT_TRUE(handshake_and_deliver(
                pull, server_pub, denied_pub, denied_sec,
                std::chrono::milliseconds(1000)));

            // Phase 5: previously-allowed peer now denied.
            EXPECT_FALSE(handshake_and_deliver(
                pull, server_pub, allowed_pub, allowed_sec,
                std::chrono::milliseconds(300)));
        },
        "zap_router::handshake_accept_deny_cycle",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int unknown_domain_denies(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            const auto [server_pub, server_sec] = make_keypair();
            const auto [client_pub, client_sec] = make_keypair();
            auto pull = bind_pull_server(server_pub, server_sec,
                                          "test.zap.unregistered.domain");

            // Register a DIFFERENT domain so the module is loaded but
            // our pull socket's domain is unknown to the router.
            InMemoryAdmission unrelated;
            PeerAllowlist     al;
            al.unrestricted = true;
            (void) unrelated.set_peer_allowlist(std::move(al));
            auto handle = ZapRouter::instance().register_domain(
                "test.zap.other.domain", &unrelated);

            ZapPumpThread pump;

            EXPECT_FALSE(handshake_and_deliver(
                pull, server_pub, client_pub, client_sec,
                std::chrono::milliseconds(300)));
        },
        "zap_router::unknown_domain_denies",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int handle_unregisters_on_destruction(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            InMemoryAdmission a;
            {
                auto h = ZapRouter::instance().register_domain(
                    "test.zap.handle.lifetime", &a);
                EXPECT_TRUE(h.is_active());
                EXPECT_EQ(ZapRouter::instance()
                              .registered_domain_count_for_test(), 1u);
            }
            EXPECT_EQ(ZapRouter::instance()
                          .registered_domain_count_for_test(), 0u);
        },
        "zap_router::handle_unregisters_on_destruction",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int duplicate_registration_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            InMemoryAdmission a;
            InMemoryAdmission b;
            auto h = ZapRouter::instance().register_domain(
                "test.zap.duplicate.domain", &a);
            EXPECT_THROW(ZapRouter::instance().register_domain(
                             "test.zap.duplicate.domain", &b),
                         std::runtime_error);
            EXPECT_EQ(ZapRouter::instance()
                          .registered_domain_count_for_test(), 1u);
        },
        "zap_router::duplicate_registration_throws",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int empty_domain_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            InMemoryAdmission a;
            EXPECT_THROW(
                ZapRouter::instance().register_domain("", &a),
                std::runtime_error);
        },
        "zap_router::empty_domain_throws",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int null_admission_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            EXPECT_THROW(
                ZapRouter::instance().register_domain("test.zap.x", nullptr),
                std::runtime_error);
        },
        "zap_router::null_admission_throws",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int pump_one_when_unloaded_returns_false(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            // No register_domain has been called → ZapRouter dynamic
            // module is NOT yet loaded → impl_->sock is empty →
            // pump_one returns false immediately.  Pins the
            // module-not-loaded contract.
            EXPECT_FALSE(ZapRouter::instance().pump_one(
                std::chrono::milliseconds(10)));
        },
        "zap_router::pump_one_when_unloaded_returns_false",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ── Dispatcher + registrar ─────────────────────────────────────────────────

int dispatch_zap_router(int argc, char **argv)
{
    if (argc < 2) return -1;
    std::string mode = argv[1];
    const auto dot = mode.find('.');
    if (dot == std::string::npos) return -1;
    const std::string module   = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "zap_router") return -1;

    if (argc < 3)
    {
        std::fprintf(stderr, "zap_router.%s: missing <tmpdir> arg\n",
                     scenario.c_str());
        return 1;
    }
    const char *tmpdir = argv[2];

    if (scenario == "handshake_accept_deny_cycle")
        return handshake_accept_deny_cycle(tmpdir);
    if (scenario == "unknown_domain_denies")
        return unknown_domain_denies(tmpdir);
    if (scenario == "handle_unregisters_on_destruction")
        return handle_unregisters_on_destruction(tmpdir);
    if (scenario == "duplicate_registration_throws")
        return duplicate_registration_throws(tmpdir);
    if (scenario == "empty_domain_throws")
        return empty_domain_throws(tmpdir);
    if (scenario == "null_admission_throws")
        return null_admission_throws(tmpdir);
    if (scenario == "pump_one_when_unloaded_returns_false")
        return pump_one_when_unloaded_returns_false(tmpdir);
    std::fprintf(stderr, "zap_router: unknown scenario '%s'\n",
                 scenario.c_str());
    return 1;
}

struct ZapRouterWorkerRegistrar
{
    ZapRouterWorkerRegistrar()
    {
        ::register_worker_dispatcher(dispatch_zap_router);
    }
};
static ZapRouterWorkerRegistrar g_zap_router_registrar;

} // namespace
