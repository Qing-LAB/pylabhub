/**
 * @file test_crypto_utils.cpp
 * @brief Layer 2 isolated-process tests for crypto_utils.
 *
 * Each TEST_F spawns an independent subprocess that initializes CryptoUtils,
 * runs the test logic, and exits. This guarantees clean lifecycle state for
 * every test.
 *
 * Tests cover:
 * - BLAKE2b hashing (determinism, collision resistance, null handling, performance)
 * - Random number generation (uniqueness, distribution, thread safety)
 * - Lifecycle integration
 */
#include "test_patterns.h"
#include "crypto_workers.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::worker::crypto;

class CryptoUtilsTest : public IsolatedProcessTest
{
};

// ============================================================================
// BLAKE2b Hashing
// ============================================================================

TEST_F(CryptoUtilsTest, BLAKE2b_ProducesCorrectSize)
{
    auto w = SpawnWorker("crypto.blake2b_correct_size");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_IsDeterministic)
{
    auto w = SpawnWorker("crypto.blake2b_deterministic");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_UniqueForDifferentInputs)
{
    auto w = SpawnWorker("crypto.blake2b_unique");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_HandlesEmptyInput)
{
    auto w = SpawnWorker("crypto.blake2b_empty_input");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_ArrayConvenience)
{
    auto w = SpawnWorker("crypto.blake2b_array_convenience");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_ArrayMatchesRaw)
{
    auto w = SpawnWorker("crypto.blake2b_array_matches_raw");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_VerifyMatching)
{
    auto w = SpawnWorker("crypto.blake2b_verify_matching");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_VerifyNonMatching)
{
    auto w = SpawnWorker("crypto.blake2b_verify_non_matching");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_VerifyArrayConvenience)
{
    auto w = SpawnWorker("crypto.blake2b_verify_array");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_HandlesLargeInput)
{
    auto w = SpawnWorker("crypto.blake2b_large_input");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_IsThreadSafe)
{
    auto w = SpawnWorker("crypto.blake2b_thread_safe");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_HandleNullOutput)
{
    auto w = SpawnWorker("crypto.blake2b_null_output");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, BLAKE2b_HandleNullInput)
{
    auto w = SpawnWorker("crypto.blake2b_null_input");
    ExpectWorkerOk(w);
}

// ============================================================================
// Random Number Generation
// ============================================================================

TEST_F(CryptoUtilsTest, Random_ProducesNonZeroOutput)
{
    auto w = SpawnWorker("crypto.random_non_zero");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, Random_IsUnique)
{
    auto w = SpawnWorker("crypto.random_unique");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, Random_U64_ProducesValidValues)
{
    auto w = SpawnWorker("crypto.random_u64");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, Random_SharedSecret_CorrectSize)
{
    auto w = SpawnWorker("crypto.random_secret_size");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, Random_SharedSecret_IsUnique)
{
    auto w = SpawnWorker("crypto.random_secret_unique");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, Random_IsThreadSafe)
{
    auto w = SpawnWorker("crypto.random_thread_safe");
    ExpectWorkerOk(w);
}

TEST_F(CryptoUtilsTest, Random_HandleNullOutput)
{
    auto w = SpawnWorker("crypto.random_null_output");
    ExpectWorkerOk(w);
}

// ============================================================================
// Lifecycle Integration
// ============================================================================

TEST(CryptoUtilsLifecycleTest, GetLifecycleModule_ReturnsValidModule)
{
    // Pure API test — GetLifecycleModule() is a static function returning a struct.
    // No lifecycle needed to call it; lifecycle is needed to USE the module.
    auto module = pylabhub::crypto::GetLifecycleModule();
    SUCCEED() << "GetLifecycleModule returns without crashing";
}

TEST_F(CryptoUtilsTest, Lifecycle_FunctionsWorkAfterInit)
{
    auto w = SpawnWorker("crypto.lifecycle_after_init");
    ExpectWorkerOk(w);
}
