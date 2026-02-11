// tests/test_layer2_service/crypto_workers.h
#pragma once
/**
 * @file crypto_workers.h
 * @brief Worker function declarations for crypto_utils isolated-process tests.
 */

namespace pylabhub::tests::worker::crypto
{

// BLAKE2b hashing workers
int blake2b_correct_size();
int blake2b_deterministic();
int blake2b_unique_for_different_inputs();
int blake2b_handles_empty_input();
int blake2b_array_convenience();
int blake2b_array_matches_raw();
int blake2b_verify_matching();
int blake2b_verify_non_matching();
int blake2b_verify_array_convenience();
int blake2b_handles_large_input();
int blake2b_is_thread_safe();
int blake2b_handle_null_output();
int blake2b_handle_null_input();

// Random generation workers
int random_produces_non_zero_output();
int random_is_unique();
int random_u64_produces_valid_values();
int random_shared_secret_correct_size();
int random_shared_secret_is_unique();
int random_is_thread_safe();
int random_handle_null_output();

// Lifecycle workers
int lifecycle_functions_work_after_init();

} // namespace pylabhub::tests::worker::crypto
