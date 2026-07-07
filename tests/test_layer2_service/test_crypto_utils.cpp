/**
 * @file test_crypto_utils.cpp
 * @brief Layer 2 isolated-process tests for the Category 1 (sodium
 *        primitives) surface on `SecureSubsystem` (HEP-CORE-0043 §2.1).
 *
 * Each TEST_F spawns an independent subprocess that stands up
 * SecureSubsystem, runs the test logic, and exits.  Guarantees clean
 * lifecycle state per test.
 *
 * Tests cover the SMS Category 1 primitives (post-2026-07-06 SecureSubsystem
 * merger, HEP-CORE-0043 §2.1 category 1a/1b):
 * - BLAKE2b hashing (determinism, collision resistance, null handling)
 * - Random number generation (uniqueness, distribution, thread safety)
 * - Retired-module lifecycle stub (empty pass-through)
 *
 * Historical: the `pylabhub::crypto` free-function namespace was folded
 * into `SecureSubsystem` methods (Category 1) on 2026-07-06.  These
 * tests were renamed `SecureSubsystemTest` → `SMS_PrimitivesTest` to
 * reflect the current subject.  Worker bodies still live in
 * `workers/crypto_workers.cpp`.
 */
#include "test_patterns.h"
#include "crypto_workers.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::worker::crypto;

class SMS_PrimitivesTest : public IsolatedProcessTest
{
};

// ============================================================================
// BLAKE2b Hashing
// ============================================================================

TEST_F(SMS_PrimitivesTest, BLAKE2b_ProducesCorrectSize)
{
    auto w = SpawnWorker("crypto.blake2b_correct_size");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_IsDeterministic)
{
    auto w = SpawnWorker("crypto.blake2b_deterministic");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_UniqueForDifferentInputs)
{
    auto w = SpawnWorker("crypto.blake2b_unique");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_HandlesEmptyInput)
{
    auto w = SpawnWorker("crypto.blake2b_empty_input");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_ArrayConvenience)
{
    auto w = SpawnWorker("crypto.blake2b_array_convenience");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_ArrayMatchesRaw)
{
    auto w = SpawnWorker("crypto.blake2b_array_matches_raw");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_VerifyMatching)
{
    auto w = SpawnWorker("crypto.blake2b_verify_matching");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_VerifyNonMatching)
{
    auto w = SpawnWorker("crypto.blake2b_verify_non_matching");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_VerifyArrayConvenience)
{
    auto w = SpawnWorker("crypto.blake2b_verify_array");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_HandlesLargeInput)
{
    auto w = SpawnWorker("crypto.blake2b_large_input");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_IsThreadSafe)
{
    auto w = SpawnWorker("crypto.blake2b_thread_safe");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_HandleNullOutput)
{
    auto w = SpawnWorker("crypto.blake2b_null_output");
    ExpectWorkerOk(w, {}, {"compute_blake2b: null pointer argument"});
}

TEST_F(SMS_PrimitivesTest, BLAKE2b_HandleNullInput)
{
    auto w = SpawnWorker("crypto.blake2b_null_input");
    ExpectWorkerOk(w, {}, {"compute_blake2b: null pointer argument"});
}

// ============================================================================
// Random Number Generation
// ============================================================================

TEST_F(SMS_PrimitivesTest, Random_ProducesNonZeroOutput)
{
    auto w = SpawnWorker("crypto.random_non_zero");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, Random_IsUnique)
{
    auto w = SpawnWorker("crypto.random_unique");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, Random_U64_ProducesValidValues)
{
    auto w = SpawnWorker("crypto.random_u64");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, Random_SharedSecret_CorrectSize)
{
    auto w = SpawnWorker("crypto.random_secret_size");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, Random_SharedSecret_IsUnique)
{
    auto w = SpawnWorker("crypto.random_secret_unique");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, Random_IsThreadSafe)
{
    auto w = SpawnWorker("crypto.random_thread_safe");
    ExpectWorkerOk(w);
}

TEST_F(SMS_PrimitivesTest, Random_HandleNullOutput)
{
    auto w = SpawnWorker("crypto.random_null_output");
    ExpectWorkerOk(w, {}, {"random_bytes: null output pointer"});
}

// ============================================================================
// Lifecycle Integration
// ============================================================================
//
// Note: there is no standalone "GetLifecycleModule_ReturnsValidModule" test.
// `ModuleDef` is a builder type with no public getters beyond
// `userdata_key()`, so a test that merely calls the factory and inspects the
// returned object cannot prove anything meaningful.  Removed 2026-04-22.
// The Lifecycle_FunctionsWorkAfterInit worker below exercises the returned
// module through a real LifecycleGuard — if `GetLifecycleModule()` ever
// returned a broken definition, that test would fail at init time.

TEST_F(SMS_PrimitivesTest, Lifecycle_FunctionsWorkAfterInit)
{
    auto w = SpawnWorker("crypto.lifecycle_after_init");
    ExpectWorkerOk(w);
}
