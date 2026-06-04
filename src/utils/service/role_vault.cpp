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

#include "vault_crypto.hpp"

#include "utils/json_fwd.hpp"
#include <zmq.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
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
    std::string role_uid_;   ///< Role UID from payload
    std::string public_key_;  ///< Z85, 40 chars
    std::string secret_key_;  ///< Z85, 40 chars
};

// ============================================================================
// Helpers
// ============================================================================

namespace
{

constexpr std::size_t kZ85BufSize = 41; // 40 printable chars + null terminator
constexpr std::size_t kZ85KeyLen  = 40;

} // anonymous namespace

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
    v.pImpl->role_uid_  = role_uid;
    v.pImpl->public_key_ = pub_str;
    v.pImpl->secret_key_ = sec_str;
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

    // vault_read throws on wrong password or I/O failure.
    const std::string plaintext = detail::vault_read(vault_path, password, role_uid);

    RoleVault v;
    try
    {
        const json j = json::parse(plaintext);
        v.pImpl->role_uid_  = j.at("role_uid").get<std::string>();
        v.pImpl->public_key_ = j.at("public_key").get<std::string>();
        v.pImpl->secret_key_ = j.at("secret_key").get<std::string>();
    }
    catch (const json::exception &e)
    {
        throw std::runtime_error(std::string("RoleVault: vault payload invalid: ") + e.what());
    }

    if (v.pImpl->public_key_.size() != kZ85KeyLen || v.pImpl->secret_key_.size() != kZ85KeyLen)
    {
        throw std::runtime_error("RoleVault: vault contains invalid key lengths (expected 40-char Z85)");
    }

    return v;
}

// ============================================================================
// Accessors
// ============================================================================

const std::string &RoleVault::public_key() const noexcept { return pImpl->public_key_; }
const std::string &RoleVault::secret_key() const noexcept { return pImpl->secret_key_; }
const std::string &RoleVault::role_uid()  const noexcept { return pImpl->role_uid_;  }

} // namespace pylabhub::utils
