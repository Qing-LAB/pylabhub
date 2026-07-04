/**
 * @file crypto_utils.cpp
 * @brief Implementation of cryptographic utilities using libsodium.
 */
#include "utils/crypto_utils.hpp"
#include "utils/logger.hpp"
#include "utils/timeout_constants.hpp"

#include <sodium.h>
#include <chrono>
#include <cstring>

namespace pylabhub::crypto
{
// sodium_init is SecureMemorySubsystem's job (HEP-CORE-0040 §4.0).
// Previous self-init + per-call gate removed 2026-07-04.

// ============================================================================
// BLAKE2b Hashing Implementation
// ============================================================================

bool compute_blake2b(uint8_t *out, const void *data, size_t len) noexcept
{
    if (out == nullptr || data == nullptr)
    {
        LOGGER_ERROR("[CryptoUtils] compute_blake2b: null pointer argument");
        return false;
    }

    // Use crypto_generichash (BLAKE2b) with no key
    int result = crypto_generichash(out,                // output hash
                                    BLAKE2B_HASH_BYTES, // hash length (32 bytes)
                                    static_cast<const unsigned char *>(data), // input data
                                    len,                                      // input length
                                    nullptr,                                  // key (none)
                                    0                                         // key length (0)
    );

    if (result != 0)
    {
        LOGGER_ERROR("[CryptoUtils] crypto_generichash failed (should never happen)");
        return false;
    }

    return true;
}

std::array<uint8_t, BLAKE2B_HASH_BYTES> compute_blake2b_array(const void *data, size_t len) noexcept
{
    std::array<uint8_t, BLAKE2B_HASH_BYTES> hash{};

    if (!compute_blake2b(hash.data(), data, len))
    {
        // Return all-zeros on failure
        hash.fill(0);
    }

    return hash;
}

bool verify_blake2b(const uint8_t *stored, const void *data, size_t len) noexcept
{
    if (stored == nullptr || data == nullptr)
    {
        LOGGER_ERROR("[CryptoUtils] verify_blake2b: null pointer argument");
        return false;
    }

    // Compute hash of data
    std::array<uint8_t, BLAKE2B_HASH_BYTES> computed{};
    if (!compute_blake2b(computed.data(), data, len))
    {
        return false;
    }

    // Constant-time comparison (timing-attack resistant)
    int cmp = sodium_memcmp(stored, computed.data(), BLAKE2B_HASH_BYTES);

    return (cmp == 0);
}

bool verify_blake2b(const std::array<uint8_t, BLAKE2B_HASH_BYTES> &stored, const void *data,
                    size_t len) noexcept
{
    return verify_blake2b(stored.data(), data, len);
}

// ============================================================================
// Random Number Generation Implementation
// ============================================================================

void generate_random_bytes(uint8_t *out, size_t len) noexcept
{
    // Null check must come before any dereference (including the memset fallback below).
    if (out == nullptr)
    {
        LOGGER_ERROR("[CryptoUtils] generate_random_bytes: null output pointer");
        return;
    }

    // libsodium's randombytes_buf never fails (will abort on catastrophic RNG failure).
    // sodium_init is SecureMemorySubsystem's job; caller must have SMS up.
    randombytes_buf(out, len);
}

uint64_t generate_random_u64() noexcept
{
    uint64_t value = 0;
    generate_random_bytes(reinterpret_cast<uint8_t *>(&value), sizeof(value));
    return value;
}

namespace
{
constexpr size_t kSharedSecretBytes = 64;
}

std::array<uint8_t, kSharedSecretBytes> generate_shared_secret() noexcept
{
    std::array<uint8_t, kSharedSecretBytes> secret{};
    generate_random_bytes(secret.data(), secret.size());
    return secret;
}

// ============================================================================
// Lifecycle Integration
// ============================================================================

namespace
{
/**
 * @brief Lifecycle startup callback for CryptoUtils module.
 * @details Initializes libsodium at application startup.
 * @param arg Unused argument (required by LifecycleCallback signature).
 */
void crypto_startup(const char *arg, void * /*userdata*/)
{
    (void)arg; // Unused

    LOGGER_DEBUG("[CryptoUtils] Module starting up...");
    // sodium_init is SecureMemorySubsystem's job (HEP-CORE-0040 §4.0).
    LOGGER_INFO("[CryptoUtils] Module initialized successfully");
}

/**
 * @brief Lifecycle shutdown callback for CryptoUtils module.
 * @details Libsodium does not require explicit cleanup, but we log shutdown for consistency.
 * @param arg Unused argument (required by LifecycleCallback signature).
 */
void crypto_shutdown(const char *arg, void * /*userdata*/)
{
    (void)arg; // Unused

    LOGGER_DEBUG("[CryptoUtils] Module shutting down...");
    // Libsodium does not require explicit cleanup, and sodium_init() is irrevocable —
    // resetting g_sodium_initialized to false would be misleading (and could race with
    // concurrent ensure_sodium_ready() calls post-shutdown). Leave it as true.
    LOGGER_INFO("[CryptoUtils] Module shutdown complete");
}

} // anonymous namespace

pylabhub::utils::ModuleDef GetLifecycleModule()
{
    pylabhub::utils::ModuleDef module("CryptoUtils");
    module.set_startup(crypto_startup);
    module.set_shutdown(crypto_shutdown, std::chrono::milliseconds(pylabhub::kShortTimeoutMs));
    // No dependencies - CryptoUtils is a foundational module
    return module;
}

} // namespace pylabhub::crypto
