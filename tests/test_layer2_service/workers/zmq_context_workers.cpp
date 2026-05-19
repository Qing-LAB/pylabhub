/**
 * @file zmq_context_workers.cpp
 * @brief Worker implementations for ZmqContext lifecycle tests.
 *
 * Each worker wraps its body in run_gtest_worker() so the LifecycleGuard
 * (Logger + hub::ZMQContext) is owned by the subprocess.
 *
 * What is and is NOT verified
 * ---------------------------
 * `run_gtest_worker` runs the test body, then LifecycleGuard's destructor,
 * then _exit(). The destructor invokes hub::ZMQContext::shutdown which
 * calls zmq_ctx_term — so init/finalize callback correctness is exercised
 * exactly as in production. _exit only skips libzmq's own static
 * destructors (its internal globals' destructors, not our finalize).
 * Those are owned by libzmq itself and are not part of any contract that
 * a pylabhub module registered with LifecycleManager. If a future test
 * needs to assert clean process exit including third-party statics, use
 * run_worker_bare (no implicit _exit) plus a tight ctest TIMEOUT.
 *
 * See docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md § "Testing
 * implications" for the full reasoning.
 */
#include "zmq_context_workers.h"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/zmq_context.hpp"
#include "utils/zmq_socket_policy.hpp"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <thread>
#include <vector>

using pylabhub::hub::GetZMQContextModule;
using pylabhub::hub::get_zmq_context;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace zmq_context
{

int get_context_returns_valid()
{
    return run_gtest_worker(
        [&]()
        {
            EXPECT_NO_THROW({
                zmq::context_t &ctx = get_zmq_context();
                (void)ctx;
            });
        },
        "zmq_context::get_context_returns_valid",
        Logger::GetLifecycleModule(), GetZMQContextModule());
}

int get_context_same_instance()
{
    return run_gtest_worker(
        [&]()
        {
            zmq::context_t &ctx1 = get_zmq_context();
            zmq::context_t &ctx2 = get_zmq_context();
            EXPECT_EQ(&ctx1, &ctx2)
                << "get_zmq_context() should return the same instance";
        },
        "zmq_context::get_context_same_instance",
        Logger::GetLifecycleModule(), GetZMQContextModule());
}

int create_socket_works()
{
    return run_gtest_worker(
        [&]()
        {
            zmq::context_t &ctx = get_zmq_context();
            EXPECT_NO_THROW({
                zmq::socket_t sock(ctx, zmq::socket_type::pair);
                sock.close();
            });
        },
        "zmq_context::create_socket_works",
        Logger::GetLifecycleModule(), GetZMQContextModule());
}

int apply_socket_policy_tcp_connect_sets_all()
{
    return run_gtest_worker(
        [&]()
        {
            // Construct a real DEALER socket on the real shared ZMQ
            // context — exact production setup pattern.  Apply the
            // helper; read back every option libzmq exposes; pin
            // each value.  Mutation: dropping ANY of the six
            // `sock.set(...)` calls in `apply_socket_policy` →
            // exactly one EXPECT_EQ in this test fails (precise
            // mutation localization).
            zmq::context_t &ctx = get_zmq_context();
            zmq::socket_t sock(ctx, zmq::socket_type::dealer);
            ::pylabhub::utils::apply_socket_policy(
                sock, ::pylabhub::utils::ZmqSocketRole::TcpConnect);

            EXPECT_EQ(sock.get(zmq::sockopt::linger), 0);
            EXPECT_EQ(sock.get(zmq::sockopt::sndtimeo), 500);
            EXPECT_EQ(sock.get(zmq::sockopt::heartbeat_ivl), 5000);
            EXPECT_EQ(sock.get(zmq::sockopt::heartbeat_timeout), 30000);
            EXPECT_EQ(sock.get(zmq::sockopt::reconnect_ivl), -1)
                << "TcpConnect MUST disable auto-reconnect "
                   "(HEP-CORE-0023 §2.5.3 'Disconnection is terminal').";
            EXPECT_EQ(sock.get(zmq::sockopt::reconnect_ivl_max), 0);

            sock.close();
        },
        "zmq_context::apply_socket_policy_tcp_connect_sets_all",
        Logger::GetLifecycleModule(), GetZMQContextModule());
}

int apply_socket_policy_tcp_bind_subset()
{
    return run_gtest_worker(
        [&]()
        {
            // Bind-side socket: heartbeat + sndtimeo + linger ARE
            // set; reconnect_ivl is left at libzmq's default (NOT
            // -1).  Verifies role-based branching in the helper.
            zmq::context_t &ctx = get_zmq_context();
            zmq::socket_t sock(ctx, zmq::socket_type::router);
            ::pylabhub::utils::apply_socket_policy(
                sock, ::pylabhub::utils::ZmqSocketRole::TcpBind);

            EXPECT_EQ(sock.get(zmq::sockopt::linger), 0);
            EXPECT_EQ(sock.get(zmq::sockopt::sndtimeo), 500);
            EXPECT_EQ(sock.get(zmq::sockopt::heartbeat_ivl), 5000);
            EXPECT_EQ(sock.get(zmq::sockopt::heartbeat_timeout), 30000);
            EXPECT_NE(sock.get(zmq::sockopt::reconnect_ivl), -1)
                << "TcpBind MUST NOT set reconnect_ivl — bind-side "
                   "sockets don't initiate, so the option is a no-op "
                   "and setting it would signal confused intent.  "
                   "Mutation: helper unconditionally setting "
                   "reconnect_ivl=-1 regardless of role → this fails.";

            sock.close();
        },
        "zmq_context::apply_socket_policy_tcp_bind_subset",
        Logger::GetLifecycleModule(), GetZMQContextModule());
}

int apply_socket_policy_inproc_signal_minimal()
{
    return run_gtest_worker(
        [&]()
        {
            // InprocSignal: ONLY linger=0.  Heartbeat, sndtimeo,
            // reconnect_ivl all left at libzmq's defaults (ZMTP
            // heartbeat is TCP-only; inproc is sub-microsecond and
            // sndtimeo bounding makes no sense; reconnect is moot
            // for inproc which isn't subject to TCP teardown).
            zmq::context_t &ctx = get_zmq_context();
            zmq::socket_t sock(ctx, zmq::socket_type::pair);
            ::pylabhub::utils::apply_socket_policy(
                sock, ::pylabhub::utils::ZmqSocketRole::InprocSignal);

            EXPECT_EQ(sock.get(zmq::sockopt::linger), 0)
                << "InprocSignal MUST still set linger=0 (clean shutdown).";
            // sndtimeo default is -1 (block forever).  Verify the
            // helper did NOT set 500 for inproc sockets.
            EXPECT_EQ(sock.get(zmq::sockopt::sndtimeo), -1)
                << "InprocSignal MUST NOT set sndtimeo (inproc is "
                   "sub-microsecond — bounding makes no sense).  "
                   "Mutation: helper applying sndtimeo=500 for inproc → "
                   "this fails.";

            sock.close();
        },
        "zmq_context::apply_socket_policy_inproc_signal_minimal",
        Logger::GetLifecycleModule(), GetZMQContextModule());
}

int multithread_get_context_safe()
{
    return run_gtest_worker(
        [&]()
        {
            constexpr int kThreads = 4;
            std::vector<zmq::context_t *> results(kThreads, nullptr);
            std::vector<std::thread>      threads;

            for (int i = 0; i < kThreads; ++i)
            {
                threads.emplace_back(
                    [&results, i]() { results[i] = &get_zmq_context(); });
            }
            for (auto &t : threads)
                t.join();

            for (int i = 1; i < kThreads; ++i)
            {
                EXPECT_EQ(results[i], results[0])
                    << "thread " << i
                    << " got a different context pointer than thread 0";
            }
        },
        "zmq_context::multithread_get_context_safe",
        Logger::GetLifecycleModule(), GetZMQContextModule());
}

} // namespace zmq_context
} // namespace pylabhub::tests::worker

namespace
{

struct ZmqContextWorkerRegistrar
{
    ZmqContextWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "zmq_context")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::zmq_context;

                if (sc == "get_context_returns_valid")
                    return get_context_returns_valid();
                if (sc == "get_context_same_instance")
                    return get_context_same_instance();
                if (sc == "create_socket_works")
                    return create_socket_works();
                if (sc == "multithread_get_context_safe")
                    return multithread_get_context_safe();
                if (sc == "apply_socket_policy_tcp_connect_sets_all")
                    return apply_socket_policy_tcp_connect_sets_all();
                if (sc == "apply_socket_policy_tcp_bind_subset")
                    return apply_socket_policy_tcp_bind_subset();
                if (sc == "apply_socket_policy_inproc_signal_minimal")
                    return apply_socket_policy_inproc_signal_minimal();

                fmt::print(stderr,
                           "[zmq_context] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static ZmqContextWorkerRegistrar g_zmq_context_registrar;

} // namespace
