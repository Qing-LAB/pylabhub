/**
 * @file role_vault.cpp
 * @brief RoleVault — encrypted actor keypair store.
 *
 * Crypto layer delegated to vault_crypto.hpp (shared with HubVault).
 * This file handles only actor-specific payload (CurveZMQ keypair).
 */
#include "utils/role_vault.hpp"

#include "vault_crypto.hpp"

#include "utils/json_fwd.hpp"
#include <zmq.h>

#include <array>
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
    std::string actor_uid_;   ///< Actor UID from payload
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
                               const std::string &actor_uid,
                               const std::string &password)
{
    detail::vault_require_sodium();

    // Generate CurveZMQ keypair (Z85).
    std::array<char, kZ85BufSize> pub{};
    std::array<char, kZ85BufSize> sec{};
    if (zmq_curve_keypair(pub.data(), sec.data()) != 0)
    {
        throw std::runtime_error("RoleVault: zmq_curve_keypair failed");
    }
    const std::string pub_str(pub.data(), kZ85KeyLen);
    const std::string sec_str(sec.data(), kZ85KeyLen);
    sodium_memzero(sec.data(), sec.size()); // zero secret key stack buffer after copying to sec_str
    sodium_memzero(pub.data(), pub.size()); // zero public key stack buffer after copying to pub_str

    // Ensure parent directory exists.
    if (vault_path.has_parent_path())
    {
        std::error_code ec;
        fs::create_directories(vault_path.parent_path(), ec);
        if (ec)
            throw std::runtime_error("RoleVault: cannot create parent directory for '" +
                                     vault_path.string() + "': " + ec.message());
    }

    // Serialize and encrypt payload.
    const json payload = {
        {"actor_uid",  actor_uid},
        {"public_key", pub_str},
        {"secret_key", sec_str}
    };
    detail::vault_write(vault_path, payload.dump(), password, actor_uid);

    RoleVault v;
    v.pImpl->actor_uid_  = actor_uid;
    v.pImpl->public_key_ = pub_str;
    v.pImpl->secret_key_ = sec_str;
    return v;
}

// ============================================================================
// RoleVault::open
// ============================================================================

RoleVault RoleVault::open(const fs::path    &vault_path,
                             const std::string &actor_uid,
                             const std::string &password)
{
    detail::vault_require_sodium();

    // vault_read throws on wrong password or I/O failure.
    const std::string plaintext = detail::vault_read(vault_path, password, actor_uid);

    RoleVault v;
    try
    {
        const json j = json::parse(plaintext);
        v.pImpl->actor_uid_  = j.at("actor_uid").get<std::string>();
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
const std::string &RoleVault::actor_uid()  const noexcept { return pImpl->actor_uid_;  }

} // namespace pylabhub::utils
