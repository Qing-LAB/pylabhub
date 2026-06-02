/**
 * @file test_zmq_queue_auth.cpp
 * @brief Pattern 3 driver — ZmqQueue CURVE+ZAP auth (PeerAdmission Phase C).
 *
 * Each TEST_F spawns a subprocess.  Workers live in
 * `workers/zmq_queue_auth_workers.cpp`.
 */
#include "test_patterns.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;

namespace
{

class ZmqQueueAuthTest : public IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    std::string unique_dir(const char *test_name)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_zmq_auth_" + std::string(test_name) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

TEST_F(ZmqQueueAuthTest, AllowedPeer_DeliversRoundTrip)
{
    auto w = SpawnWorker("zmq_queue_auth.auth_round_trip_allowed_peer_delivers",
                         {unique_dir("auth_round_trip_allowed_peer_delivers")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, UnallowedPeer_BlockedFromDelivery)
{
    auto w = SpawnWorker("zmq_queue_auth.auth_unallowed_peer_blocked",
                         {unique_dir("auth_unallowed_peer_blocked")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, AllowlistSwap_TakesEffectForNextConnection)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_allowlist_swap_takes_effect_for_next_connection",
        {unique_dir("auth_allowlist_swap_takes_effect_for_next_connection")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, LegacyUnauthFactories_Unchanged)
{
    auto w = SpawnWorker("zmq_queue_auth.legacy_unauth_factories_unchanged",
                         {unique_dir("legacy_unauth_factories_unchanged")});
    ExpectWorkerOk(w);
}
