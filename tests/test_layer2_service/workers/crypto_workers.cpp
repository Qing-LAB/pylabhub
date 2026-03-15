// tests/test_layer2_service/crypto_workers.cpp
#include "crypto_workers.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "plh_service.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace pylabhub::crypto;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::crypto
{

static auto crypto_module()
{
    return pylabhub::crypto::GetLifecycleModule();
}

// ============================================================================
// BLAKE2b Hashing
// ============================================================================

int blake2b_correct_size()
{
    return run_gtest_worker(
        []()
        {
            const char *input = "test data";
            uint8_t hash[BLAKE2B_HASH_BYTES];
            ASSERT_TRUE(compute_blake2b(hash, input, strlen(input)));
            bool all_zero = true;
            for (size_t i = 0; i < BLAKE2B_HASH_BYTES; ++i)
                if (hash[i] != 0)
                {
                    all_zero = false;
                    break;
                }
            EXPECT_FALSE(all_zero) << "Hash should not be all zeros";
        },
        "blake2b_correct_size", crypto_module());
}

int blake2b_deterministic()
{
    return run_gtest_worker(
        []()
        {
            const char *input = "The quick brown fox jumps over the lazy dog";
            uint8_t hash1[BLAKE2B_HASH_BYTES], hash2[BLAKE2B_HASH_BYTES];
            ASSERT_TRUE(compute_blake2b(hash1, input, strlen(input)));
            ASSERT_TRUE(compute_blake2b(hash2, input, strlen(input)));
            EXPECT_EQ(0, std::memcmp(hash1, hash2, BLAKE2B_HASH_BYTES))
                << "BLAKE2b must be deterministic";
        },
        "blake2b_deterministic", crypto_module());
}

int blake2b_unique_for_different_inputs()
{
    return run_gtest_worker(
        []()
        {
            const char *input1 = "test data 1", *input2 = "test data 2";
            uint8_t hash1[BLAKE2B_HASH_BYTES], hash2[BLAKE2B_HASH_BYTES];
            ASSERT_TRUE(compute_blake2b(hash1, input1, strlen(input1)));
            ASSERT_TRUE(compute_blake2b(hash2, input2, strlen(input2)));
            EXPECT_NE(0, std::memcmp(hash1, hash2, BLAKE2B_HASH_BYTES))
                << "Different inputs must produce different hashes";
        },
        "blake2b_unique", crypto_module());
}

int blake2b_handles_empty_input()
{
    return run_gtest_worker(
        []()
        {
            uint8_t hash[BLAKE2B_HASH_BYTES];
            EXPECT_TRUE(compute_blake2b(hash, "", 0));
            bool all_zero = true;
            for (size_t i = 0; i < BLAKE2B_HASH_BYTES; ++i)
                if (hash[i] != 0)
                {
                    all_zero = false;
                    break;
                }
            EXPECT_FALSE(all_zero) << "Empty input should produce valid hash";
        },
        "blake2b_empty_input", crypto_module());
}

int blake2b_array_convenience()
{
    return run_gtest_worker(
        []()
        {
            const char *input = "test data";
            auto hash = compute_blake2b_array(input, strlen(input));
            bool all_zero = true;
            for (uint8_t byte : hash)
                if (byte != 0)
                {
                    all_zero = false;
                    break;
                }
            EXPECT_FALSE(all_zero);
        },
        "blake2b_array_convenience", crypto_module());
}

int blake2b_array_matches_raw()
{
    return run_gtest_worker(
        []()
        {
            const char *input = "test data";
            uint8_t hash_raw[BLAKE2B_HASH_BYTES];
            compute_blake2b(hash_raw, input, strlen(input));
            auto hash_array = compute_blake2b_array(input, strlen(input));
            EXPECT_EQ(0, std::memcmp(hash_raw, hash_array.data(), BLAKE2B_HASH_BYTES))
                << "Array version should match raw version";
        },
        "blake2b_array_matches_raw", crypto_module());
}

int blake2b_verify_matching()
{
    return run_gtest_worker(
        []()
        {
            const char *input = "test data";
            uint8_t hash[BLAKE2B_HASH_BYTES];
            compute_blake2b(hash, input, strlen(input));
            EXPECT_TRUE(verify_blake2b(hash, input, strlen(input)))
                << "Verification should succeed for matching hash";
        },
        "blake2b_verify_matching", crypto_module());
}

int blake2b_verify_non_matching()
{
    return run_gtest_worker(
        []()
        {
            const char *input1 = "test data 1", *input2 = "test data 2";
            uint8_t hash1[BLAKE2B_HASH_BYTES];
            compute_blake2b(hash1, input1, strlen(input1));
            EXPECT_FALSE(verify_blake2b(hash1, input2, strlen(input2)))
                << "Verification should fail for non-matching hash";
        },
        "blake2b_verify_non_matching", crypto_module());
}

int blake2b_verify_array_convenience()
{
    return run_gtest_worker(
        []()
        {
            const char *input = "test data";
            auto hash = compute_blake2b_array(input, strlen(input));
            EXPECT_TRUE(verify_blake2b(hash, input, strlen(input)))
                << "Array verification should work";
        },
        "blake2b_verify_array", crypto_module());
}

int blake2b_handles_large_input()
{
    return run_gtest_worker(
        []()
        {
            const size_t large_size = 1024 * 1024;
            std::vector<uint8_t> large_data(large_size, 0x42);
            uint8_t hash[BLAKE2B_HASH_BYTES];
            auto start = std::chrono::high_resolution_clock::now();
            ASSERT_TRUE(compute_blake2b(hash, large_data.data(), large_data.size()));
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::high_resolution_clock::now() - start)
                          .count();
            EXPECT_LT(ms, 100) << "BLAKE2b should be fast for 1MB";
        },
        "blake2b_large_input", crypto_module());
}

int blake2b_is_thread_safe()
{
    return run_gtest_worker(
        []()
        {
            const int n_threads = 10, hashes_per = 100;
            std::atomic<int> success{0};
            ThreadRacer racer(n_threads);
            racer.race(
                [&](int t)
                {
                    for (int i = 0; i < hashes_per; ++i)
                    {
                        std::string input = "t" + std::to_string(t) + "_h" + std::to_string(i);
                        uint8_t hash[BLAKE2B_HASH_BYTES];
                        if (compute_blake2b(hash, input.data(), input.size()))
                            success.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            EXPECT_EQ(success.load(), n_threads * hashes_per)
                << "All hash ops should succeed under concurrent load";
        },
        "blake2b_thread_safe", crypto_module());
}

int blake2b_handle_null_output()
{
    return run_gtest_worker(
        []()
        {
            EXPECT_FALSE(compute_blake2b(nullptr, "data", 4))
                << "Should fail gracefully with null output";
        },
        "blake2b_null_output", crypto_module());
}

int blake2b_handle_null_input()
{
    return run_gtest_worker(
        []()
        {
            uint8_t hash[BLAKE2B_HASH_BYTES];
            EXPECT_FALSE(compute_blake2b(hash, nullptr, 10))
                << "Should fail gracefully with null input";
        },
        "blake2b_null_input", crypto_module());
}

// ============================================================================
// Random Number Generation
// ============================================================================

int random_produces_non_zero_output()
{
    return run_gtest_worker(
        []()
        {
            uint8_t random[64];
            generate_random_bytes(random, sizeof(random));
            bool all_zero = true;
            for (uint8_t byte : random)
                if (byte != 0)
                {
                    all_zero = false;
                    break;
                }
            EXPECT_FALSE(all_zero) << "Random output should not be all zeros";
        },
        "random_non_zero", crypto_module());
}

int random_is_unique()
{
    return run_gtest_worker(
        []()
        {
            const int n = 100;
            std::set<std::string> samples;
            for (int i = 0; i < n; ++i)
            {
                uint8_t r[32];
                generate_random_bytes(r, sizeof(r));
                samples.insert(std::string(reinterpret_cast<char *>(r), sizeof(r)));
            }
            EXPECT_EQ(samples.size(), static_cast<size_t>(n))
                << "Random generation should produce unique values";
        },
        "random_unique", crypto_module());
}

int random_u64_produces_valid_values()
{
    return run_gtest_worker(
        []()
        {
            std::set<uint64_t> vals;
            for (int i = 0; i < 100; ++i)
                vals.insert(generate_random_u64());
            EXPECT_GT(vals.size(), 90u) << "Random u64 should produce mostly unique values";
        },
        "random_u64", crypto_module());
}

int random_shared_secret_correct_size()
{
    return run_gtest_worker(
        []()
        {
            auto secret = generate_shared_secret();
            EXPECT_EQ(secret.size(), 64u);
            bool all_zero = true;
            for (uint8_t b : secret)
                if (b != 0)
                {
                    all_zero = false;
                    break;
                }
            EXPECT_FALSE(all_zero) << "Shared secret should be random";
        },
        "random_secret_size", crypto_module());
}

int random_shared_secret_is_unique()
{
    return run_gtest_worker(
        []()
        {
            EXPECT_NE(generate_shared_secret(), generate_shared_secret())
                << "Different shared secrets should be unique";
        },
        "random_secret_unique", crypto_module());
}

int random_is_thread_safe()
{
    return run_gtest_worker(
        []()
        {
            const int n_threads = 10, per = 100;
            std::vector<std::vector<uint64_t>> vals(n_threads);
            ThreadRacer racer(n_threads);
            racer.race(
                [&](int t)
                {
                    for (int i = 0; i < per; ++i)
                        vals[t].push_back(generate_random_u64());
                });
            std::set<uint64_t> all;
            for (auto &v : vals)
                for (uint64_t x : v)
                    all.insert(x);
            EXPECT_GT(all.size(), static_cast<size_t>(n_threads * per) * 99 / 100)
                << "Random generation should be thread-safe with high uniqueness";
        },
        "random_thread_safe", crypto_module());
}

int random_handle_null_output()
{
    return run_gtest_worker([]() { EXPECT_NO_THROW(generate_random_bytes(nullptr, 64)); },
                            "random_null_output", crypto_module());
}

// ============================================================================
// Lifecycle
// ============================================================================

int lifecycle_functions_work_after_init()
{
    return run_gtest_worker(
        []()
        {
            uint8_t hash[BLAKE2B_HASH_BYTES];
            EXPECT_TRUE(compute_blake2b(hash, "test", 4));
            EXPECT_GT(generate_random_u64(), 0u);
        },
        "lifecycle_functions_after_init", crypto_module());
}

} // namespace pylabhub::tests::worker::crypto

// Self-registering dispatcher — no separate dispatcher file needed.
namespace
{
struct CryptoWorkerRegistrar
{
    CryptoWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "crypto")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::crypto;
                if (scenario == "blake2b_correct_size")
                    return blake2b_correct_size();
                if (scenario == "blake2b_deterministic")
                    return blake2b_deterministic();
                if (scenario == "blake2b_unique")
                    return blake2b_unique_for_different_inputs();
                if (scenario == "blake2b_empty_input")
                    return blake2b_handles_empty_input();
                if (scenario == "blake2b_array_convenience")
                    return blake2b_array_convenience();
                if (scenario == "blake2b_array_matches_raw")
                    return blake2b_array_matches_raw();
                if (scenario == "blake2b_verify_matching")
                    return blake2b_verify_matching();
                if (scenario == "blake2b_verify_non_matching")
                    return blake2b_verify_non_matching();
                if (scenario == "blake2b_verify_array")
                    return blake2b_verify_array_convenience();
                if (scenario == "blake2b_large_input")
                    return blake2b_handles_large_input();
                if (scenario == "blake2b_thread_safe")
                    return blake2b_is_thread_safe();
                if (scenario == "blake2b_null_output")
                    return blake2b_handle_null_output();
                if (scenario == "blake2b_null_input")
                    return blake2b_handle_null_input();
                if (scenario == "random_non_zero")
                    return random_produces_non_zero_output();
                if (scenario == "random_unique")
                    return random_is_unique();
                if (scenario == "random_u64")
                    return random_u64_produces_valid_values();
                if (scenario == "random_secret_size")
                    return random_shared_secret_correct_size();
                if (scenario == "random_secret_unique")
                    return random_shared_secret_is_unique();
                if (scenario == "random_thread_safe")
                    return random_is_thread_safe();
                if (scenario == "random_null_output")
                    return random_handle_null_output();
                if (scenario == "lifecycle_after_init")
                    return lifecycle_functions_work_after_init();
                fmt::print(stderr, "ERROR: Unknown crypto scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static CryptoWorkerRegistrar g_crypto_registrar;
} // namespace
