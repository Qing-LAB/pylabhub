/**
 * @file role_vault.cpp
 * @brief RoleVault — encrypted role keypair store.
 *
 * Crypto layer delegated to vault_crypto.hpp (shared with HubVault).
 * This file handles only role-specific payload (CurveZMQ keypair).
 */
// `curve_keypair.hpp` and `secure_buffer.hpp` are gone from this file:
// the keypair is minted inside the key store now, and there is no
// plaintext buffer here to wipe because the vault layer owns it.
#include "utils/role_vault.hpp"
#include "utils/security/key_file_acl.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"

#include "vault_crypto.hpp"

#include "utils/json_fwd.hpp"
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
using json = nlohmann::json;

namespace pylabhub::utils
{

// ============================================================================
// Pimpl
// ============================================================================

struct RoleVault::Impl
{
    /// Nothing in here is secret any more.
    ///
    /// The secret key used to live beside the public one as a 40-char
    /// Z85 array, zeroed in this destructor.  Under HEP-CORE-0035
    /// §4.6.6 the vault deposits it straight into the key store and
    /// hands the caller metadata only, so there is no secret member to
    /// zero and no window in which this object holds one.  That is
    /// strictly better than wiping carefully: the bytes are never here.
    std::array<char, 40> public_z85{};
    std::string role_uid_;

    static constexpr std::size_t kKeyLen = 40;
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
RoleVault::~RoleVault() = default;
RoleVault::RoleVault(RoleVault &&) noexcept = default;
RoleVault &RoleVault::operator=(RoleVault &&) noexcept = default;

// ============================================================================
// RoleVault::create
// ============================================================================

RoleVault RoleVault::create(const fs::path &vault_path, const std::string &role_uid,
                            const std::string &password, std::string_view identity_name,
                            std::string_view key_name)
{
    namespace sec = pylabhub::utils::security;

    // Mint the keypair INSIDE the key store.  `generate_and_add_identity`
    // returns the public half and nothing else, so the secret has no
    // representation out here to leak — the previous shape pulled both
    // halves out as `std::string`s and relied on remembering to wipe
    // them, which it did not do.
    const std::string pub_str = sec::secure().keys().generate_and_add_identity(identity_name);

    // Minting comes first because the secret must exist before it can be
    // written, but everything after this point can fail — the parent
    // directory, the 0700 chmod, and the O_EXCL write that refuses an
    // existing vault.  A create that fails must not leave a half-made
    // identity in the key store: the next attempt would then fail with
    // "name already present" instead of the real reason, and a caller
    // would be holding a key for a vault that does not exist.
    struct DropIdentityOnFailure
    {
        std::string_view name;
        bool armed = true;
        ~DropIdentityOnFailure() noexcept
        {
            if (!armed)
                return;
            try
            {
                pylabhub::utils::security::secure().keys().remove(name);
            }
            catch (...)
            {
                // Already gone, or the store is unusable.  Either way
                // there is nothing useful to do while unwinding.
            }
        }
    } drop_identity{identity_name};

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
        const auto rc =
            sec::set_keyfile_mode(vault_path.parent_path(), sec::KeyFileRole::VaultDir, &chmod_err);
        // set_keyfile_mode always populates out_errno on ChmodFailed
        // (chmod failure → errno; bad_alloc fallback → ENOMEM).
        if (rc == sec::SetModeResult::ChmodFailed)
            throw std::runtime_error("RoleVault: chmod 0700 failed on vault parent dir '" +
                                     vault_path.parent_path().string() +
                                     "': " + std::strerror(chmod_err));
    }

    if (pub_str.size() != Impl::kKeyLen)
    {
        throw std::runtime_error("RoleVault::create: generated key has unexpected length");
    }

    // The metadata carries the PUBLIC half only (VF-6).  The secret is
    // not a field here in any encoding — it is copied raw out of the key
    // store by the callback below, straight into the buffer the AEAD
    // encrypts over.
    const json metadata = {{"role_uid", role_uid}, {"public_key", pub_str}};

    detail::vault_add_key_from_password(key_name, password, role_uid);
    detail::vault_write(vault_path, detail::VaultKind::Role, metadata.dump(), key_name,
                        [&](std::span<std::uint8_t> secret_section)
                        {
                            sec::secure().keys().with_seckey(
                                identity_name,
                                [&](std::string_view raw_seckey)
                                {
                                    if (raw_seckey.size() != secret_section.size())
                                    {
                                        throw std::runtime_error(
                                            "RoleVault::create: key store returned a " +
                                            std::to_string(raw_seckey.size()) +
                                            "-byte secret for a " +
                                            std::to_string(secret_section.size()) +
                                            "-byte secret section");
                                    }
                                    std::memcpy(secret_section.data(), raw_seckey.data(),
                                                secret_section.size());
                                });
                        });

    drop_identity.armed = false; // the vault exists; the identity belongs to it

    RoleVault v;
    v.pImpl->role_uid_ = role_uid;
    std::memcpy(v.pImpl->public_z85.data(), pub_str.data(), Impl::kKeyLen);
    return v;
}

// ============================================================================
// RoleVault::open
// ============================================================================

RoleVault RoleVault::open(const fs::path &vault_path, const std::string &role_uid,
                          const std::string &password, std::string_view identity_name,
                          std::string_view key_name)
{
    namespace sec = pylabhub::utils::security;

    RoleVault v;
    detail::vault_read(
        vault_path, detail::VaultKind::Role, role_uid, password, key_name,
        [&](std::span<const std::uint8_t> secret, std::string_view metadata_json)
        {
            std::string pub_z85;
            try
            {
                const json j = json::parse(metadata_json.begin(), metadata_json.end());
                v.pImpl->role_uid_ = j.at("role_uid").get<std::string>();
                pub_z85 = j.at("public_key").get<std::string>();
            }
            catch (const json::exception &e)
            {
                throw std::runtime_error(std::string("RoleVault: vault metadata invalid: ") +
                                         e.what());
            }

            // Nothing above touched a secret — the metadata has none
            // (VF-6), so ordinary `std::string` handling of it is fine
            // and no wipe guard is needed.  That is the whole point of
            // moving the secret out of the document.
            if (pub_z85.size() != Impl::kKeyLen)
            {
                throw std::runtime_error(
                    "RoleVault: vault metadata has an invalid public key length (expected "
                    "40-char Z85)");
            }
            std::memcpy(v.pImpl->public_z85.data(), pub_z85.data(), Impl::kKeyLen);

            // Deposit the identity: public half decoded from the
            // metadata's Z85, secret half copied raw from the plaintext
            // (VF-7).  `add_identity` wants them packed pub‖sec, and
            // zeroes the buffer we hand it.
            std::array<std::byte, 64> packed{};
            struct WipePacked
            {
                std::array<std::byte, 64> &b;
                ~WipePacked() noexcept
                {
                    pylabhub::utils::security::secure().memzero(std::span<std::uint8_t>(
                        reinterpret_cast<std::uint8_t *>(b.data()), b.size()));
                }
            } wipe_packed{packed};

            std::uint8_t pub_raw[32]{};
            if (::zmq_z85_decode(pub_raw, pub_z85.c_str()) == nullptr)
            {
                throw std::runtime_error("RoleVault: vault metadata public key is not valid Z85");
            }
            std::memcpy(packed.data(), pub_raw, sizeof(pub_raw));
            std::memcpy(packed.data() + sizeof(pub_raw), secret.data(), secret.size());
            sec::secure().keys().add_identity(identity_name, std::span<std::byte>(packed));
        });

    return v;
}

// ============================================================================
// Accessors
// ============================================================================

std::string_view RoleVault::public_key() const noexcept
{
    return std::string_view(pImpl->public_z85.data(), Impl::kKeyLen);
}
std::string_view RoleVault::role_uid() const noexcept
{
    return pImpl->role_uid_;
}

} // namespace pylabhub::utils
