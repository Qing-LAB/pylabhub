#pragma once
/**
 * @file crypto_utils.hpp
 * @brief Cryptographic utilities for checksums, hashing, and random number generation.
 *
 * This module provides a centralized interface for cryptographic operations used
 * throughout pyLabHub, including:
 * - BLAKE2b hashing for data integrity (checksums)
 * - Random byte generation for shared secrets and capabilities
 * - Future: Encryption/decryption for sensitive data
 *
 * All cryptographic primitives are provided by libsodium, which is initialized
 * automatically via the Lifecycle module.
 *
 * Design Rationale:
 * - Single point of libsodium initialization (via Lifecycle)
 * - Reusable across DataBlock, MessageHub, JsonConfig, Logger
 * - ABI-stable interface (no libsodium types in public API)
 * - Thread-safe (libsodium is thread-safe after initialization)
 *
 * @see https://libsodium.gitbook.io/doc/
 */
#include "pylabhub_utils_export.h"
#include "module_def.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>

namespace pylabhub::crypto
{

// ============================================================================
// Constants
// ============================================================================

/** BLAKE2b hash output size in bytes (256-bit = 32 bytes). */
static constexpr size_t BLAKE2B_HASH_BYTES = 32;

/** Default personalization string for BLAKE2b hashing (ensures domain separation). */
static constexpr char BLAKE2B_PERSONALIZATION[] = "PYLABHUB_V1_2026";

// ============================================================================
// BLAKE2b Hashing
// ============================================================================

/**
 * @brief Computes a BLAKE2b-256 hash of the input data.
 * @details Uses libsodium's crypto_generichash (BLAKE2b) to compute a
 *          cryptographically secure hash. BLAKE2b is faster than SHA-256
 *          and provides the same security level.
 *
 * @param out Pointer to output buffer (must be at least BLAKE2B_HASH_BYTES).
 * @param data Pointer to input data to hash.
 * @param len Length of input data in bytes.
 * @return True on success, false if libsodium initialization failed.
 *
 * @note Thread-safe after libsodium initialization.
 * @note No key is used (unkeyed hash for checksums, not MACs).
 *
 * @example
 * uint8_t hash[BLAKE2B_HASH_BYTES];
 * if (compute_blake2b(hash, data_ptr, data_size)) {
 *     // Use hash for integrity checking
 * }
 */
PYLABHUB_UTILS_EXPORT bool compute_blake2b(uint8_t *out, const void *data, size_t len) noexcept;

/**
 * @brief Computes a BLAKE2b-256 hash and returns it as a std::array.
 * @details Convenience wrapper that returns the hash by value for easier
 *          storage and comparison.
 *
 * @param data Input data to hash.
 * @param len Length of input data in bytes.
 * @return std::array containing the 32-byte hash, or all zeros on failure.
 *
 * @example
 * auto hash = compute_blake2b_array(data_ptr, data_size);
 * if (hash != std::array<uint8_t, 32>{}) { // Check for non-zero
 *     // Use hash
 * }
 */
PYLABHUB_UTILS_EXPORT std::array<uint8_t, BLAKE2B_HASH_BYTES>
compute_blake2b_array(const void *data, size_t len) noexcept;

/**
 * @brief Verifies that a stored hash matches the computed hash of data.
 * @details Computes BLAKE2b hash of data and compares it with the stored
 *          hash using constant-time comparison (timing-attack resistant).
 *
 * @param stored Pointer to stored hash (must be BLAKE2B_HASH_BYTES).
 * @param data Pointer to data to verify.
 * @param len Length of data in bytes.
 * @return True if hashes match, false otherwise.
 *
 * @note Uses sodium_memcmp() for constant-time comparison.
 *
 * @example
 * if (verify_blake2b(stored_checksum, slot_data, slot_size)) {
 *     // Data integrity verified
 * } else {
 *     // Corruption detected!
 * }
 */
PYLABHUB_UTILS_EXPORT bool verify_blake2b(const uint8_t *stored, const void *data,
                                          size_t len) noexcept;

/**
 * @brief Verifies a hash using std::array for stored hash.
 * @details Convenience wrapper for verify_blake2b() that accepts std::array.
 *
 * @param stored The stored hash as a std::array.
 * @param data Pointer to data to verify.
 * @param len Length of data in bytes.
 * @return True if hashes match, false otherwise.
 */
PYLABHUB_UTILS_EXPORT bool verify_blake2b(const std::array<uint8_t, BLAKE2B_HASH_BYTES> &stored,
                                          const void *data, size_t len) noexcept;

// ============================================================================
// Random Number Generation
// ============================================================================

/**
 * @brief Generates cryptographically secure random bytes.
 * @details Uses libsodium's randombytes_buf() which provides unpredictable
 *          random data suitable for cryptographic keys and secrets.
 *
 * @param out Pointer to output buffer to fill with random bytes.
 * @param len Number of random bytes to generate.
 *
 * @note Thread-safe and fork-safe (libsodium handles fork() correctly).
 * @note Never fails (libsodium will abort on catastrophic RNG failure).
 *
 * @example
 * uint8_t secret[64];
 * generate_random_bytes(secret, sizeof(secret));
 * // Use secret for authentication
 */
PYLABHUB_UTILS_EXPORT void generate_random_bytes(uint8_t *out, size_t len) noexcept;

/**
 * @brief Generates a random 64-bit unsigned integer.
 * @details Convenience function for generating random IDs, sequence numbers, etc.
 *
 * @return A cryptographically secure random uint64_t.
 *
 * @example
 * uint64_t session_id = generate_random_u64();
 */
PYLABHUB_UTILS_EXPORT uint64_t generate_random_u64() noexcept;

/**
 * @brief Generates a random shared secret as a std::array.
 * @details Convenience function for generating DataBlock shared secrets.
 *
 * @return A 64-byte random secret.
 *
 * @example
 * auto secret = generate_shared_secret();
 * // Store in DataBlockConfig or SharedMemoryHeader
 */
PYLABHUB_UTILS_EXPORT std::array<uint8_t, 64> generate_shared_secret() noexcept;

// ============================================================================
// Lifecycle Integration
// ============================================================================

/**
 * @brief Returns the ModuleDef for crypto utilities lifecycle management.
 * @details Registers libsodium initialization/finalization with the Lifecycle system.
 *          The startup function calls sodium_init() once at application startup.
 *
 * @return ModuleDef for "CryptoUtils" module with no dependencies.
 *
 * @note Must be registered with LifecycleManager before using any crypto functions.
 *
 * @example
 * int main() {
 *     pylabhub::utils::LifecycleGuard lifecycle(
 *         pylabhub::utils::MakeModDefList(
 *             pylabhub::crypto::GetLifecycleModule()
 *         )
 *     );
 *     // Safe to use crypto functions now
 * }
 */
PYLABHUB_UTILS_EXPORT pylabhub::utils::ModuleDef GetLifecycleModule();

} // namespace pylabhub::crypto
