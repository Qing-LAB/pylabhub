/**
 * @file role_vault.cpp
 * @brief RoleVault — encrypted role keypair store.
 *
 * Crypto layer delegated to vault_crypto.hpp (shared with HubVault).
 * This file handles only role-specific payload (CurveZMQ keypair).
 */
#include "utils/role_vault.hpp"
#include "utils/security/curve_keypair.hpp"
#include "utils/security/key_file_acl.hpp"
#include "utils/security/secure_buffer.hpp"

#include "vault_crypto.hpp"

#include "utils/json_fwd.hpp"
#include <sodium.h>
#include <zmq.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace pylabhub::utils
{

// ============================================================================
// Pimpl
// ============================================================================

struct RoleVault::Impl
{
    /// HEP-CORE-0040 §175: secrets stored in fixed-size buffers + zeroed
    /// in dtor via `sodium_memzero`.  Replaces the pre-#175
    /// `std::string` members whose destructors are not guaranteed to
    /// zero heap memory.
    std::array<char, 40>  public_z85{};
    std::array<char, 40>  secret_z85{};
    std::string           role_uid_;  ///< Role UID is not secret; std::string OK.

    static constexpr std::size_t kKeyLen = 40;

    ~Impl() noexcept
    {
        sodium_memzero(public_z85.data(), public_z85.size());
        sodium_memzero(secret_z85.data(), secret_z85.size());
        // role_uid_ is not secret — no zero needed, std::string dtor
        // releases the heap memory normally.
    }
};

// ============================================================================
// Helpers
// ============================================================================

// (no anon-namespace constants needed — all sizes live as
// `RoleVault::Impl::kKeyLen` and the JSON field-name string literals
// are used inline.)

// ============================================================================
// Constructors / destructor
// ============================================================================

RoleVault::RoleVault() : pImpl(std::make_unique<Impl>()) {}
RoleVault::~RoleVault()                          = default;
RoleVault::RoleVault(RoleVault &&) noexcept     = default;
RoleVault &RoleVault::operator=(RoleVault &&) noexcept = default;

// ============================================================================
// RoleVault::create
// ============================================================================

RoleVault RoleVault::create(const fs::path    &vault_path,
                               const std::string &role_uid,
                               const std::string &password)
{
    detail::vault_require_sodium();

    // Generate CurveZMQ keypair (Z85).
    auto kp = pylabhub::utils::security::generate_curve_keypair();
    const std::string pub_str = std::move(kp.public_z85);
    const std::string sec_str = std::move(kp.secret_z85);

    // Ensure parent directory exists.
    if (vault_path.has_parent_path())
    {
        std::error_code ec;
        fs::create_directories(vault_path.parent_path(), ec);
        if (ec)
            throw std::runtime_error("RoleVault: cannot create parent directory for '" +
                                     vault_path.string() + "': " + ec.message());
        // HEP-CORE-0035 §4.6.1: keystore directory MUST be 0700.
        // fs::create_directories applies process umask; enforce
        // explicit 0700 here so the directory is not briefly
        // world-readable before vault_write lays down 0600 inside.
        namespace sec = pylabhub::utils::security;
        int chmod_err = 0;
        const auto rc = sec::set_keyfile_mode(
            vault_path.parent_path(), sec::KeyFileRole::VaultDir, &chmod_err);
        // set_keyfile_mode always populates out_errno on ChmodFailed
        // (chmod failure → errno; bad_alloc fallback → ENOMEM).
        if (rc == sec::SetModeResult::ChmodFailed)
            throw std::runtime_error(
                "RoleVault: chmod 0700 failed on vault parent dir '" +
                vault_path.parent_path().string() + "': " +
                std::strerror(chmod_err));
    }

    // Serialize and encrypt payload.
    const json payload = {
        {"role_uid",  role_uid},
        {"public_key", pub_str},
        {"secret_key", sec_str}
    };
    detail::vault_write(vault_path, payload.dump(), password, role_uid);

    RoleVault v;
    if (pub_str.size() != Impl::kKeyLen || sec_str.size() != Impl::kKeyLen)
    {
        throw std::runtime_error(
            "RoleVault::create: generated keys have unexpected length");
    }
    v.pImpl->role_uid_     = role_uid;
    std::memcpy(v.pImpl->public_z85.data(), pub_str.data(), Impl::kKeyLen);
    std::memcpy(v.pImpl->secret_z85.data(), sec_str.data(), Impl::kKeyLen);
    return v;
}

// ============================================================================
// RoleVault::open
// ============================================================================

RoleVault RoleVault::open(const fs::path    &vault_path,
                             const std::string &role_uid,
                             const std::string &password)
{
    detail::vault_require_sodium();

    // HEP-CORE-0040 §175: decrypt into a stack buffer whose destructor
    // zeroes the plaintext when this scope exits.
    pylabhub::utils::security::SecureBuffer<4096> json_buf;
    const std::size_t n =
        detail::vault_read_secure(vault_path, password, role_uid, json_buf.span());

    RoleVault v;
    try
    {
        const auto bytes = json_buf.span().first(n);
        const json j = json::parse(
            reinterpret_cast<const char *>(bytes.data()),
            reinterpret_cast<const char *>(bytes.data() + bytes.size()));

        // role_uid is not secret — std::string copy is fine.
        v.pImpl->role_uid_ = j.at("role_uid").get<std::string>();

        // Keys go through a temporary std::string into the fixed-size
        // zero-on-destruct buffers.  The temporaries' heap storage
        // exists for microseconds while we copy; identity bytes live
        // in the process KeyStore (locked memory) from
        // `RoleConfig::load_keypair` onward per HEP-CORE-0040 §172.
        const auto pub_str = j.at("public_key").get<std::string>();
        const auto sec_str = j.at("secret_key").get<std::string>();
        if (pub_str.size() != Impl::kKeyLen || sec_str.size() != Impl::kKeyLen)
        {
            throw std::runtime_error(
                "RoleVault: vault contains invalid key lengths (expected 40-char Z85)");
        }
        std::memcpy(v.pImpl->public_z85.data(), pub_str.data(), Impl::kKeyLen);
        std::memcpy(v.pImpl->secret_z85.data(), sec_str.data(), Impl::kKeyLen);
    }
    catch (const json::exception &e)
    {
        throw std::runtime_error(std::string("RoleVault: vault payload invalid: ") + e.what());
    }

    return v;
}

// ============================================================================
// Accessors
// ============================================================================

std::string_view RoleVault::public_key() const noexcept
{
    return std::string_view(pImpl->public_z85.data(), Impl::kKeyLen);
}
std::string_view RoleVault::secret_key() const noexcept
{
    return std::string_view(pImpl->secret_z85.data(), Impl::kKeyLen);
}
std::string_view RoleVault::role_uid()  const noexcept
{
    return pImpl->role_uid_;
}

} // namespace pylabhub::utils
