/**
 * @file test_key_store.cpp
 * @brief Pattern 3 driver — KeyStore L2 test suite (HEP-CORE-0040 §170).
 *
 * Each TEST_F spawns a fresh subprocess that owns its own LifecycleGuard
 * (Logger).  Subprocess isolation is required because
 * `SecureMemorySubsystem` and `KeyStore` are PROCESS SINGLETONS (HEP-0040
 * §4.1 + §5.1) — running multiple scenarios serially in one process
 * would have the second ctor throw "already constructed."
 *
 * Worker bodies in `workers/key_store_workers.cpp`.
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

class KeyStoreTest : public IsolatedProcessTest
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
                     ("plh_l2_keystore_" + std::string(test_name) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

// ── Lifecycle / singleton invariants ───────────────────────────────────────

TEST_F(KeyStoreTest, OrderingCheck_KeyStoreRequiresSecureMemory)
{
    auto w = SpawnWorker(
        "key_store.ordering_check_keystore_requires_secure_memory",
        {unique_dir("ordering")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, SecondConstruction_Throws)
{
    auto w = SpawnWorker("key_store.second_construction_throws",
                         {unique_dir("second_ctor")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, KeyStoreNotInitialized_Throws)
{
    auto w = SpawnWorker("key_store.key_store_not_initialized_throws",
                         {unique_dir("not_init")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, KeyStoreReady_ProbeLifecycle)
{
    auto w = SpawnWorker("key_store.key_store_ready_probe_lifecycle",
                         {unique_dir("ready_probe")});
    ExpectWorkerOk(w);
}

// ── Identity-key happy paths ───────────────────────────────────────────────

TEST_F(KeyStoreTest, AddIdentity_PubkeyAndWithSeckeyRoundtrip)
{
    auto w = SpawnWorker(
        "key_store.add_identity_then_pubkey_and_with_seckey_roundtrip",
        {unique_dir("identity_roundtrip")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, AddIdentity_ZerosSource)
{
    auto w = SpawnWorker("key_store.add_identity_zeros_source",
                         {unique_dir("zeros_source")});
    ExpectWorkerOk(w);
}

// ── Error paths — missing / wrong-type / duplicate / wrong-size ────────────

TEST_F(KeyStoreTest, Pubkey_OnMissing_Throws)
{
    auto w = SpawnWorker("key_store.pubkey_on_missing_throws",
                         {unique_dir("pubkey_missing")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, WithSeckey_OnMissing_Throws)
{
    auto w = SpawnWorker("key_store.with_seckey_on_missing_throws",
                         {unique_dir("seckey_missing")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, LookupRaw_OnMissing_Throws)
{
    auto w = SpawnWorker("key_store.lookup_raw_on_missing_throws",
                         {unique_dir("raw_missing")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, Pubkey_OnRawEntry_Throws)
{
    auto w = SpawnWorker("key_store.pubkey_on_raw_entry_throws",
                         {unique_dir("pubkey_raw_type")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, WithSeckey_OnRawEntry_Throws)
{
    auto w = SpawnWorker("key_store.with_seckey_on_raw_entry_throws",
                         {unique_dir("seckey_raw_type")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, AddIdentity_Duplicate_Throws)
{
    auto w = SpawnWorker("key_store.add_identity_duplicate_throws",
                         {unique_dir("duplicate")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, AddIdentity_WrongSize_Throws)
{
    auto w = SpawnWorker("key_store.add_identity_wrong_size_throws",
                         {unique_dir("wrong_size")});
    ExpectWorkerOk(w);
}

// ── Raw secret (HEP-0038) happy path ───────────────────────────────────────

TEST_F(KeyStoreTest, AddRaw_LookupRawRoundtrip)
{
    auto w = SpawnWorker("key_store.add_raw_then_lookup_raw_roundtrip",
                         {unique_dir("raw_roundtrip")});
    ExpectWorkerOk(w);
}

// ── remove + has / size ────────────────────────────────────────────────────

TEST_F(KeyStoreTest, Remove_MakesSubsequentLookupThrow)
{
    auto w = SpawnWorker("key_store.remove_makes_subsequent_lookup_throw",
                         {unique_dir("remove")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, HasAndSize_TrackEntries)
{
    auto w = SpawnWorker("key_store.has_and_size_track_entries",
                         {unique_dir("has_size")});
    ExpectWorkerOk(w);
}

// ── Thread-safety — parallel reads + remove vs in-flight callback ──────────

TEST_F(KeyStoreTest, ParallelReads_DoNotBlock)
{
    auto w = SpawnWorker("key_store.parallel_reads_do_not_block",
                         {unique_dir("parallel_reads")});
    ExpectWorkerOk(w);
}

TEST_F(KeyStoreTest, Remove_BlocksBehindInFlightWithSeckey)
{
    auto w = SpawnWorker(
        "key_store.remove_blocks_behind_in_flight_with_seckey",
        {unique_dir("remove_blocks")});
    ExpectWorkerOk(w);
}

// ── SecureBuffer<N> ────────────────────────────────────────────────────────

TEST_F(KeyStoreTest, SecureBuffer_DtorZeroes)
{
    auto w = SpawnWorker("key_store.secure_buffer_dtor_zeroes",
                         {unique_dir("secure_buffer")});
    ExpectWorkerOk(w);
}
