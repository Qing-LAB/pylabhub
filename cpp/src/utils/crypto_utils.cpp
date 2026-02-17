/**
 * @file crypto_utils.cpp
 * @brief Implementation of cryptographic utilities using libsodium.
 */
#include "utils/crypto_utils.hpp"
#include "plh_service.hpp" // For Logger (includes plh_base.hpp)

#include <sodium.h>
#include <chrono>
#include <cstring>

namespace pylabhub::crypto
{

// ============================================================================
// Libsodium Initialization (Internal)
// ============================================================================

namespace
{
/**
 * @brief Internal flag to track libsodium initialization status.
 * @details Libsodium's sodium_init() is idempotent and thread-safe,
 *          but we track this for logging purposes.
 */
std::atomic<bool> g_sodium_initialized{false};

/**
 * @brief Ensures libsodium is initialized, calling sodium_init() if needed.
 * @return True if libsodium is initialized, false on catastrophic failure.
 */
bool ensure_sodium_init() noexcept
{
    // Fast path: already initialized
    if (g_sodium_initialized.load(std::memory_order_acquire))
    {
        return true;
    }

    // Slow path: initialize libsodium
    // sodium_init() is thread-safe and idempotent
    int result = sodium_init();

    if (result == -1)
    {
        // Catastrophic failure (should never happen)
        LOGGER_ERROR("[CryptoUtils] FATAL: sodium_init() failed!");
        return false;
    }

    // result == 0: first initialization
    // result == 1: already initialized by another thread
    bool first_init = (result == 0);

    g_sodium_initialized.store(true, std::memory_order_release);

    if (first_init)
    {
        LOGGER_INFO("[CryptoUtils] libsodium initialized successfully");
    }

    return true;
}

} // anonymous namespace

// ============================================================================
// BLAKE2b Hashing Implementation
// ============================================================================

bool compute_blake2b(uint8_t *out, const void *data, size_t len) noexcept
{
    if (!ensure_sodium_init())
    {
        return false;
    }

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
    if (!ensure_sodium_init())
    {
        return false;
    }

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
    if (!ensure_sodium_init())
    {
        // This should never happen, but fill with zeros for safety
        LOGGER_ERROR(
            "[CryptoUtils] FATAL: Cannot generate random bytes, libsodium not initialized!");
        std::memset(out, 0, len);
        return;
    }

    if (out == nullptr)
    {
        LOGGER_ERROR("[CryptoUtils] generate_random_bytes: null output pointer");
        return;
    }

    // libsodium's randombytes_buf never fails (will abort on catastrophic RNG failure)
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
void crypto_startup(const char *arg)
{
    (void)arg; // Unused

    LOGGER_DEBUG("[CryptoUtils] Module starting up...");

    if (!ensure_sodium_init())
    {
        LOGGER_ERROR("[CryptoUtils] FATAL: Failed to initialize libsodium!");
        // This is catastrophic - the application cannot continue
        std::abort();
    }

    LOGGER_INFO("[CryptoUtils] Module initialized successfully");
}

/**
 * @brief Lifecycle shutdown callback for CryptoUtils module.
 * @details Libsodium does not require explicit cleanup, but we log shutdown for consistency.
 * @param arg Unused argument (required by LifecycleCallback signature).
 */
void crypto_shutdown(const char *arg)
{
    (void)arg; // Unused

    LOGGER_DEBUG("[CryptoUtils] Module shutting down...");
    // Libsodium does not require explicit cleanup
    g_sodium_initialized.store(false, std::memory_order_release);
    LOGGER_INFO("[CryptoUtils] Module shutdown complete");
}

} // anonymous namespace

pylabhub::utils::ModuleDef GetLifecycleModule()
{
    pylabhub::utils::ModuleDef module("CryptoUtils");
    module.set_startup(crypto_startup);
    module.set_shutdown(crypto_shutdown, std::chrono::milliseconds(1000));
    // No dependencies - CryptoUtils is a foundational module
    return module;
}

} // namespace pylabhub::crypto
