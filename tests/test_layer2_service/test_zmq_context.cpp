/**
 * @file test_zmq_context.cpp
 * @brief Unit tests for the ZmqContext lifecycle singleton.
 *
 * Uses LifecycleGuard with GetZMQContextModule() to initialize/shutdown the
 * shared ZMQ context. Tests verify the context is valid and reusable.
 */
#include "plh_service.hpp"
#include "utils/zmq_context.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace pylabhub::utils;
using namespace pylabhub::hub;

// ============================================================================
// Fixture — LifecycleGuard with ZMQ context module
// ============================================================================

class ZmqContextTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(
            MakeModDefList(Logger::GetLifecycleModule(), GetZMQContextModule()), std::source_location::current());
    }

    static void TearDownTestSuite() { s_lifecycle_.reset(); }

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<LifecycleGuard> ZmqContextTest::s_lifecycle_;

// ============================================================================
// Tests
// ============================================================================

TEST_F(ZmqContextTest, GetContext_ReturnsValid)
{
    EXPECT_NO_THROW({
        zmq::context_t &ctx = get_zmq_context();
        (void)ctx;
    });
}

TEST_F(ZmqContextTest, GetContext_SameInstance)
{
    zmq::context_t &ctx1 = get_zmq_context();
    zmq::context_t &ctx2 = get_zmq_context();
    EXPECT_EQ(&ctx1, &ctx2) << "get_zmq_context() should return the same instance";
}

TEST_F(ZmqContextTest, CreateSocket_Works)
{
    zmq::context_t &ctx = get_zmq_context();
    EXPECT_NO_THROW({
        zmq::socket_t sock(ctx, zmq::socket_type::pair);
        sock.close();
    });
}

TEST_F(ZmqContextTest, MultiThread_GetContext_Safe)
{
    // 4 threads calling get_zmq_context() concurrently; all must return same pointer.
    constexpr int kThreads = 4;
    std::vector<zmq::context_t *> results(kThreads, nullptr);
    std::vector<std::thread> threads;

    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&results, i]() { results[i] = &get_zmq_context(); });
    }
    for (auto &t : threads)
        t.join();

    for (int i = 1; i < kThreads; ++i)
    {
        EXPECT_EQ(results[i], results[0])
            << "Thread " << i << " got a different context pointer than thread 0";
    }
}
