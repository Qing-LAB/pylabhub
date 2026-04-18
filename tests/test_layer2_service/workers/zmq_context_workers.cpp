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

                fmt::print(stderr,
                           "[zmq_context] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static ZmqContextWorkerRegistrar g_zmq_context_registrar;

} // namespace
