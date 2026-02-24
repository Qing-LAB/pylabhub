/**
 * @file hub_vault.cpp
 * @brief HubVault — encrypted hub secrets store (Argon2id + XSalsa20-Poly1305).
 */
#include "utils/hub_vault.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <zmq.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace pylabhub::utils
{

// ============================================================================
// Pimpl
// ============================================================================

struct HubVault::Impl
{
    std::string broker_secret_key; ///< Z85, 40 chars
    std::string broker_public_key; ///< Z85, 40 chars
    std::string admin_token;       ///< 64-char hex
};

// ============================================================================
// Internal helpers
// ============================================================================

namespace
{

// Vault binary layout constants
constexpr std::size_t kNonceBytes   = crypto_secretbox_NONCEBYTES; // 24
constexpr std::size_t kKeyBytes     = crypto_secretbox_KEYBYTES;   // 32
constexpr std::size_t kMacBytes     = crypto_secretbox_MACBYTES;   // 16
constexpr std::size_t kSaltBytes    = crypto_pwhash_SALTBYTES;     // 16
constexpr std::size_t kZ85BufSize   = 41; // 40 printable chars + null terminator
constexpr std::size_t kZ85KeyLen    = 40;
constexpr std::size_t kAdminRawBytes = 32; // → 64 hex chars

/// Ensure libsodium is initialised (idempotent, thread-safe).
void require_sodium()
{
    const int rc = sodium_init();
    if (rc == -1)
        throw std::runtime_error("HubVault: sodium_init() failed");
}

/// Argon2id key derivation.
/// Salt = BLAKE2b-16(hub_uid) so the KDF salt is exactly kSaltBytes (16 bytes).
std::array<uint8_t, kKeyBytes> derive_key(const std::string &password,
                                          const std::string &hub_uid)
{
    // Derive a deterministic 16-byte salt from the hub_uid string.
    uint8_t salt[kSaltBytes]{};
    crypto_generichash(salt, kSaltBytes,
                       reinterpret_cast<const uint8_t *>(hub_uid.data()), hub_uid.size(),
                       nullptr, 0);

    std::array<uint8_t, kKeyBytes> key{};
    const int rc = crypto_pwhash(key.data(), key.size(),
                                 password.data(), password.size(),
                                 salt,
                                 crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                 crypto_pwhash_MEMLIMIT_INTERACTIVE,
                                 crypto_pwhash_ALG_ARGON2ID13);
    if (rc != 0)
        throw std::runtime_error(
            "HubVault: Argon2id key derivation failed (insufficient memory?)");
    return key;
}

/// Generate 32 random bytes and return as a 64-char lowercase hex string.
std::string generate_admin_token()
{
    uint8_t raw[kAdminRawBytes]{};
    randombytes_buf(raw, sizeof(raw));

    char hex[kAdminRawBytes * 2 + 1]{};
    sodium_bin2hex(hex, sizeof(hex), raw, sizeof(raw));
    return std::string(hex, kAdminRawBytes * 2);
}

/// Write binary data to path with mode 0600 (owner read/write only).
void write_secure_file(const fs::path &path, const std::vector<uint8_t> &data)
{
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            throw std::runtime_error("HubVault: cannot write: " + path.string());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        ofs.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!ofs)
            throw std::runtime_error("HubVault: write failed: " + path.string());
    } // flush + close before chmod
    fs::permissions(path,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
}

/// Read all bytes from path into a vector.
std::vector<uint8_t> read_file(const fs::path &path)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs)
        throw std::runtime_error("HubVault: cannot open vault: " + path.string());
    const auto size = static_cast<std::size_t>(ifs.tellg());
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    ifs.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(size));
    if (!ifs)
        throw std::runtime_error("HubVault: read failed: " + path.string());
    return buf;
}

} // anonymous namespace

// ============================================================================
// HubVault — constructors / destructor
// ============================================================================

HubVault::HubVault() : pImpl(std::make_unique<Impl>()) {}
HubVault::~HubVault() = default;
HubVault::HubVault(HubVault &&) noexcept = default;
HubVault &HubVault::operator=(HubVault &&) noexcept = default;

// ============================================================================
// HubVault::create
// ============================================================================

HubVault HubVault::create(const fs::path &hub_dir,
                           const std::string &hub_uid,
                           const std::string &password)
{
    require_sodium();

    // 1. Generate broker CurveZMQ keypair (Z85).
    std::array<char, kZ85BufSize> pub{};
    std::array<char, kZ85BufSize> sec{};
    if (zmq_curve_keypair(pub.data(), sec.data()) != 0)
        throw std::runtime_error("HubVault: zmq_curve_keypair failed");
    const std::string broker_public(pub.data(), kZ85KeyLen);
    const std::string broker_secret(sec.data(), kZ85KeyLen);

    // 2. Generate random admin token.
    const std::string admin_tok = generate_admin_token();

    // 3. Serialize payload to JSON.
    const json payload = {
        {"broker",
         {{"curve_secret_key", broker_secret}, {"curve_public_key", broker_public}}},
        {"admin", {{"token", admin_tok}}}};
    const std::string plaintext = payload.dump();

    // 4. Derive encryption key from password + hub_uid.
    const auto key = derive_key(password, hub_uid);

    // 5. Random nonce.
    uint8_t nonce[kNonceBytes]{};
    randombytes_buf(nonce, kNonceBytes);

    // 6. Encrypt plaintext → [ciphertext || MAC(16 bytes)].
    const std::size_t ciphertext_len = plaintext.size() + kMacBytes;
    std::vector<uint8_t> ciphertext(ciphertext_len);
    crypto_secretbox_easy(ciphertext.data(),
                          reinterpret_cast<const uint8_t *>(plaintext.data()),
                          plaintext.size(),
                          nonce, key.data());

    // 7. Write [nonce || ciphertext] to <hub_dir>/hub.vault.
    std::vector<uint8_t> vault_bytes;
    vault_bytes.reserve(kNonceBytes + ciphertext_len);
    vault_bytes.insert(vault_bytes.end(), nonce, nonce + kNonceBytes);
    vault_bytes.insert(vault_bytes.end(), ciphertext.begin(), ciphertext.end());
    write_secure_file(hub_dir / "hub.vault", vault_bytes);

    // 8. Return populated HubVault.
    HubVault v;
    v.pImpl->broker_secret_key = broker_secret;
    v.pImpl->broker_public_key = broker_public;
    v.pImpl->admin_token       = admin_tok;
    return v;
}

// ============================================================================
// HubVault::open
// ============================================================================

HubVault HubVault::open(const fs::path &hub_dir,
                         const std::string &hub_uid,
                         const std::string &password)
{
    require_sodium();

    // 1. Read vault file.
    const auto vault_bytes = read_file(hub_dir / "hub.vault");

    constexpr std::size_t kMinSize = kNonceBytes + kMacBytes + 1;
    if (vault_bytes.size() < kMinSize)
        throw std::runtime_error("HubVault: vault file too small or corrupted");

    // 2. Split: nonce (first 24 bytes) | ciphertext (remainder).
    const uint8_t *nonce      = vault_bytes.data();
    const uint8_t *ciphertext = vault_bytes.data() + kNonceBytes;
    const std::size_t clen    = vault_bytes.size() - kNonceBytes;

    // 3. Derive key.
    const auto key = derive_key(password, hub_uid);

    // 4. Decrypt + MAC verify.  Fails if password wrong or data corrupted.
    std::vector<uint8_t> plaintext(clen - kMacBytes);
    const int rc = crypto_secretbox_open_easy(plaintext.data(), ciphertext, clen,
                                              nonce, key.data());
    if (rc != 0)
        throw std::runtime_error(
            "HubVault: decryption failed — wrong password or corrupted vault");

    // 5. Parse JSON payload.
    HubVault v;
    try
    {
        const json j = json::parse(plaintext.begin(), plaintext.end());
        v.pImpl->broker_secret_key =
            j.at("broker").at("curve_secret_key").get<std::string>();
        v.pImpl->broker_public_key =
            j.at("broker").at("curve_public_key").get<std::string>();
        v.pImpl->admin_token = j.at("admin").at("token").get<std::string>();
    }
    catch (const json::exception &e)
    {
        throw std::runtime_error(std::string("HubVault: vault payload invalid: ") +
                                 e.what());
    }

    return v;
}

// ============================================================================
// Accessors
// ============================================================================

const std::string &HubVault::broker_curve_secret_key() const noexcept
{
    return pImpl->broker_secret_key;
}

const std::string &HubVault::broker_curve_public_key() const noexcept
{
    return pImpl->broker_public_key;
}

const std::string &HubVault::admin_token() const noexcept
{
    return pImpl->admin_token;
}

// ============================================================================
// publish_public_key
// ============================================================================

void HubVault::publish_public_key(const fs::path &hub_dir) const
{
    const fs::path pubkey_path = hub_dir / "hub.pubkey";
    {
        std::ofstream ofs(pubkey_path, std::ios::trunc);
        if (!ofs)
            throw std::runtime_error("HubVault: cannot write hub.pubkey: " +
                                     pubkey_path.string());
        ofs << pImpl->broker_public_key;
        if (!ofs)
            throw std::runtime_error("HubVault: write failed: " + pubkey_path.string());
    } // flush + close before chmod
    // World-readable so actors can read it without special permissions.
    fs::permissions(pubkey_path,
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
}

} // namespace pylabhub::utils
