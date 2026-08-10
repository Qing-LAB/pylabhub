/**
 * @file hub_vault.cpp
 * @brief HubVault — encrypted hub secrets store.
 *
 * Crypto layer delegated to vault_crypto.hpp (shared with RoleVault).
 * This file handles only hub-specific payload structure (broker keypair + admin token).
 */
// `curve_keypair.hpp` and `secure_buffer.hpp` are gone from this file:
// the keypair is minted inside the key store, and the plaintext buffer
// that needed wiping is now owned by the vault layer.
#include "utils/hub_vault.hpp"
#include "plh_platform.hpp"
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
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace pylabhub::utils
{

// ============================================================================
// Pimpl
// ============================================================================

struct HubVault::Impl
{
    /// No secret lives here any more.  The broker secret key and the
    /// admin token both go straight into the key store when the vault
    /// is opened or created, so this object holds only the public half
    /// and the names needed to find the rest again.  The three
    /// zero-on-destruct arrays this replaced were careful handling of
    /// bytes that no longer need to be here at all.
    std::array<char, 40> broker_public_z85{};

    /// KeyStore names filed by `create()` / `open()`.  `save()` cites
    /// them instead of asking the caller for the password, or for the
    /// secrets, again.  Not secret — they are names.
    std::string key_name_;
    std::string identity_name_;
    std::string token_name_;

    static constexpr std::size_t kPublicLen = 40;

    /// The `known_roles` allowlist document (HEP-CORE-0035 §4.8).
    /// PUBLIC-key data — not secret — so it is a plain json member with
    /// no zero-on-destruct treatment (unlike the secrets above).  Held
    /// OPAQUELY: the vault never interprets its `{version, roles:[…]}`
    /// schema (that is `KnownRolesStore`'s job).  Defaults to an empty
    /// object = §4.8.4 deny-all bootstrap.
    json known_roles = json::object();

};

// ============================================================================
// Helpers
// ============================================================================

namespace
{

/// Split of the hub secret section — broker seckey then admin token.
/// Both halves are 32 raw bytes; the static_assert in
/// `secure_subsystem.cpp` ties the total to `kVaultHubSecretBytes`.
constexpr std::size_t kBrokerSeckeyBytes = 32;
constexpr std::size_t kAdminTokenBytes = 32;

/// Mint a fresh admin token straight into the key store under `name`.
///
/// It used to be built as a 64-char hex `std::string` and returned.  It
/// is now 32 raw bytes that never leave locked memory: `add_random_key`
/// fills a `sodium_malloc` allocation from the CSPRNG, so there is no
/// buffer here to wipe and no string for a caller to mishandle.  Hex is
/// a presentation encoding — the operator sees it once, from `--keygen`
/// — and the stored form is raw.
void mint_admin_token(std::string_view name)
{
    pylabhub::utils::security::secure().keys().add_random_key(name, kAdminTokenBytes);
}

/// Fill a hub secret section: broker seckey (32) ‖ admin token (32),
/// each copied straight out of the key store (VF-7).  One definition,
/// used by both `create()` and `save()`, so the two cannot drift into
/// writing different layouts.
void fill_hub_secret_section(std::span<std::uint8_t> section, std::string_view identity_name,
                             std::string_view token_name)
{
    namespace sec = pylabhub::utils::security;

    if (section.size() != kBrokerSeckeyBytes + kAdminTokenBytes)
    {
        throw std::runtime_error("HubVault: secret section is " +
                                 std::to_string(section.size()) + " bytes, expected " +
                                 std::to_string(kBrokerSeckeyBytes + kAdminTokenBytes));
    }

    sec::secure().keys().with_seckey(
        identity_name,
        [&](std::string_view raw)
        {
            if (raw.size() != kBrokerSeckeyBytes)
            {
                throw std::runtime_error("HubVault: key store holds a " +
                                         std::to_string(raw.size()) +
                                         "-byte broker secret, expected " +
                                         std::to_string(kBrokerSeckeyBytes));
            }
            std::memcpy(section.data(), raw.data(), kBrokerSeckeyBytes);
        });

    sec::secure().keys().with_raw_key(
        token_name,
        [&](std::span<const std::byte> raw)
        {
            if (raw.size() != kAdminTokenBytes)
            {
                throw std::runtime_error(
                    "HubVault: key store holds a " + std::to_string(raw.size()) +
                    "-byte admin token, expected " + std::to_string(kAdminTokenBytes));
            }
            std::memcpy(section.data() + kBrokerSeckeyBytes, raw.data(), kAdminTokenBytes);
        });
}

} // anonymous namespace

// ============================================================================
// Constructors / destructor
// ============================================================================

HubVault::HubVault() : pImpl(std::make_unique<Impl>()) {}
HubVault::~HubVault() = default;
HubVault::HubVault(HubVault &&) noexcept = default;
HubVault &HubVault::operator=(HubVault &&) noexcept = default;

// ============================================================================
// HubVault::create
// ============================================================================

HubVault HubVault::create(const fs::path &vault_path, const std::string &hub_uid,
                          const std::string &password, std::string_view identity_name,
                          std::string_view token_name, std::string_view key_name)
{
    namespace sec = pylabhub::utils::security;

    // Both secrets are minted INSIDE the key store.  Neither has a
    // representation out here that could be forgotten about: the
    // keypair's public half is all that comes back, and the token
    // returns nothing at all.
    const std::string broker_public =
        sec::secure().keys().generate_and_add_identity(identity_name);
    mint_admin_token(token_name);

    // Both secrets must exist before they can be written, but the write
    // can still be refused (existing vault, symlink, bad directory).  A
    // failed create must leave no trace in the key store — otherwise the
    // next attempt fails with "name already present" rather than the
    // real reason, and the process holds two secrets for a hub that was
    // never created.
    struct DropSecretsOnFailure
    {
        std::string_view identity;
        std::string_view token;
        bool armed = true;
        ~DropSecretsOnFailure() noexcept
        {
            if (!armed)
                return;
            auto drop = [](std::string_view n) noexcept
            {
                try
                {
                    pylabhub::utils::security::secure().keys().remove(n);
                }
                catch (...)
                {
                }
            };
            drop(identity);
            drop(token);
        }
    } drop_secrets{identity_name, token_name};

    // Serialize payload and encrypt at the operator-supplied path
    // (HEP-CORE-0033 §7.1 — caller resolved via resolve_keyfile_path).
    // Ensure the parent directory exists; --init creates the
    // canonical `vault/` dir but direct callers (tests, custom
    // deployment locations) may not have.
    // §4.8.4: a freshly created vault has an EMPTY known_roles map —
    // deny-all bootstrap (every REG_REQ rejected until the operator
    // runs `--add-known-role`).  Stored opaquely as an empty object.
    // Metadata carries the PUBLIC half and the allowlist only (VF-6).
    // There is no "admin"."token" field any more, and no
    // "curve_secret_key" — both are raw bytes in the secret section.
    const json metadata = {{"broker", {{"curve_public_key", broker_public}}},
                           {"known_roles", json::object()}};
    if (vault_path.has_parent_path())
    {
        fs::create_directories(vault_path.parent_path());
        // HEP-CORE-0035 §4.6.1: keystore directory MUST be 0700.
        // fs::create_directories applies process umask (typically
        // 0755 result); enforce explicit 0700 here so the directory
        // is not briefly world-readable before vault_write lays
        // down the 0600 secret inside.
        namespace sec = pylabhub::utils::security;
        int chmod_err = 0;
        const auto rc =
            sec::set_keyfile_mode(vault_path.parent_path(), sec::KeyFileRole::VaultDir, &chmod_err);
        // set_keyfile_mode always populates out_errno on ChmodFailed
        // (chmod failure → errno; bad_alloc fallback → ENOMEM).
        if (rc == sec::SetModeResult::ChmodFailed)
            throw std::runtime_error("HubVault: chmod 0700 failed on vault parent dir '" +
                                     vault_path.parent_path().string() +
                                     "': " + std::strerror(chmod_err));
    }
    if (broker_public.size() != HubVault::Impl::kPublicLen)
    {
        throw std::runtime_error("HubVault::create: generated key has unexpected length");
    }

    detail::vault_add_key_from_password(key_name, password, hub_uid);
    detail::vault_write(vault_path, detail::VaultKind::Hub, metadata.dump(), key_name,
                        [&](std::span<std::uint8_t> section)
                        { fill_hub_secret_section(section, identity_name, token_name); });

    drop_secrets.armed = false; // the vault exists; the secrets belong to it

    HubVault v;
    v.pImpl->key_name_ = std::string(key_name);
    v.pImpl->identity_name_ = std::string(identity_name);
    v.pImpl->token_name_ = std::string(token_name);
    std::memcpy(v.pImpl->broker_public_z85.data(), broker_public.data(),
                HubVault::Impl::kPublicLen);
    return v;
}

// ============================================================================
// HubVault::open
// ============================================================================

HubVault HubVault::open(const fs::path &vault_path, const std::string &hub_uid,
                        const std::string &password, std::string_view identity_name,
                        std::string_view token_name, std::string_view key_name)
{
    namespace sec = pylabhub::utils::security;

    HubVault v;
    v.pImpl->key_name_ = std::string(key_name);
    v.pImpl->identity_name_ = std::string(identity_name);
    v.pImpl->token_name_ = std::string(token_name);

    detail::vault_read(
        vault_path, detail::VaultKind::Hub, hub_uid, password, key_name,
        [&](std::span<const std::uint8_t> secret, std::string_view metadata_json)
        {
            std::string broker_public;
            try
            {
                const json j = json::parse(metadata_json.begin(), metadata_json.end());
                broker_public = j.at("broker").at("curve_public_key").get<std::string>();

                // known_roles (HEP-CORE-0035 §4.8) — PUBLIC data, held
                // opaquely.  Not zeroed: pubkeys are not secret.
                v.pImpl->known_roles =
                    j.contains("known_roles") ? j.at("known_roles") : json::object();
            }
            catch (const json::exception &e)
            {
                throw std::runtime_error(std::string("HubVault: vault metadata invalid: ") +
                                         e.what());
            }

            // Nothing above handled a secret, so ordinary `std::string`
            // extraction is fine here — the wipe guards this function
            // used to need existed only because the secrets were
            // document values.
            if (broker_public.size() != HubVault::Impl::kPublicLen)
            {
                throw std::runtime_error(
                    "HubVault: broker.curve_public_key has unexpected length (" +
                    std::to_string(broker_public.size()) + ", expected " +
                    std::to_string(HubVault::Impl::kPublicLen) + ")");
            }
            std::memcpy(v.pImpl->broker_public_z85.data(), broker_public.data(),
                        HubVault::Impl::kPublicLen);

            // Deposit the broker identity: public half decoded from the
            // metadata, secret half taken raw from the secret section.
            {
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
                if (::zmq_z85_decode(pub_raw, broker_public.c_str()) == nullptr)
                {
                    throw std::runtime_error(
                        "HubVault: broker.curve_public_key is not valid Z85");
                }
                std::memcpy(packed.data(), pub_raw, sizeof(pub_raw));
                std::memcpy(packed.data() + sizeof(pub_raw), secret.data(), kBrokerSeckeyBytes);
                sec::secure().keys().add_identity(identity_name, std::span<std::byte>(packed));
            }

            // Deposit the admin token as a raw key.  `add_raw` zeroes
            // the buffer we hand it, so the only copy outside locked
            // memory dies inside that call.
            {
                std::array<std::byte, kAdminTokenBytes> tok{};
                std::memcpy(tok.data(), secret.data() + kBrokerSeckeyBytes, kAdminTokenBytes);
                sec::secure().keys().add_raw(token_name, std::span<std::byte>(tok));
            }
        });

    return v;
}

// ============================================================================
// Accessors
// ============================================================================

std::string_view HubVault::broker_curve_public_key() const noexcept
{
    return std::string_view(pImpl->broker_public_z85.data(), Impl::kPublicLen);
}

// ============================================================================
// known_roles (HEP-CORE-0035 §4.8)
// ============================================================================

const json &HubVault::known_roles() const noexcept
{
    return pImpl->known_roles;
}

void HubVault::set_known_roles(json roles)
{
    pImpl->known_roles = std::move(roles);
}

void HubVault::save(const fs::path &vault_path) const
{
    // Rebuild the metadata from in-memory state; only known_roles
    // reflects any set_known_roles() call.  The secrets are NOT
    // reconstructed here — they are still in the key store under the
    // names open()/create() filed them under, and get copied out inside
    // the fill callback below.  The version of this function that built
    // them into the payload string put both secrets in an unwiped
    // `std::string` on every save.
    const json metadata = {
        {"broker",
         {{"curve_public_key", std::string(pImpl->broker_public_z85.data(), Impl::kPublicLen)}}},
        {"known_roles", pImpl->known_roles}};

    // ATOMIC OVERWRITE: `vault_write` (via write_secure_file) is
    // create-only — O_CREAT|O_EXCL refuses to clobber, which is correct
    // for `create()` but wrong for re-encrypting an existing vault.
    // Write the fresh ciphertext to a sibling temp (O_EXCL, fresh) then
    // rename(2) over the target.  rename is atomic on POSIX: a crash
    // leaves either the old vault or the new one intact — never a
    // truncated file.  Temp name is pid-scoped to avoid collisions.
    const fs::path tmp =
        vault_path.parent_path() / (vault_path.filename().string() + ".tmp." +
                                    std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    fs::remove(tmp, ec); // clear any stale temp from a prior crash
    detail::vault_write(tmp, detail::VaultKind::Hub, metadata.dump(), pImpl->key_name_,
                        [&](std::span<std::uint8_t> section) {
                            fill_hub_secret_section(section, pImpl->identity_name_,
                                                    pImpl->token_name_);
                        });
    fs::rename(tmp, vault_path, ec);
    if (ec)
    {
        std::error_code rmec;
        fs::remove(tmp, rmec);
        throw std::runtime_error("HubVault::save: atomic rename failed for '" +
                                 vault_path.string() + "': " + ec.message());
    }
}

// ============================================================================
// publish_public_key
// ============================================================================

void HubVault::publish_public_key(const fs::path &hub_dir) const
{
    const fs::path pubkey_path = hub_dir / "hub.pubkey";

    // The pubkey is intentionally world-readable (0644) — confidentiality
    // is not the goal here — but INTEGRITY is.  An attacker who can
    // briefly write inside hub_dir could plant a symlink at `hub.pubkey`
    // redirecting the write to a target of their choosing, which would
    // then propagate as federation trust material when the operator
    // copies the file to a role directory.  `write_keyfile` refuses to
    // follow a symlink (O_NOFOLLOW) and normalizes the mode with
    // `fchmod` on the fd rather than `chmod` on the path.
    //
    // This used to be ~90 lines of hand-written open/fchmod/write/close
    // here — a second copy of HEP-CORE-0035 §4.6.1's recipe, differing
    // from the vault's copy only in the mode.  `KeyFileRole` already
    // carried that difference, so both collapse to one call.
    //
    // `Replace` supersedes the previous unlink-then-create: `rename(2)`
    // is atomic, so a reader now sees either the old pubkey or the new
    // one.  The old sequence left a window in which `hub.pubkey` did not
    // exist at all, and a re-keygen that failed mid-write left it that
    // way.
    // Operator-visible note when we are about to overwrite an existing
    // pubkey.  This is federation trust material: on a re-keygen the
    // replacement is expected, but if the operator did not expect it,
    // the fact that a pubkey was already sitting there is the thing
    // worth investigating.  The signal is about the OVERWRITE, not
    // about the mechanism — it survived the move to atomic replace
    // because the operator's question ("was something already here?")
    // is unchanged.  Pinned by
    // PlhHubCliTest.KeygenEmitsNote_WhenPreExistingPubkeyRemoved.
    {
        std::error_code ec;
        if (fs::exists(pubkey_path, ec))
        {
            std::fprintf(stderr,
                         "[plh_hub] note: pre-existing hub.pubkey at '%s' "
                         "will be atomically replaced (publish attempt "
                         "follows; expected on re-keygen; investigate if "
                         "unexpected — HEP-CORE-0035 §4.6.4)\n",
                         pubkey_path.string().c_str());
        }
    }

    namespace sec = pylabhub::utils::security;
    sec::write_keyfile(pubkey_path, broker_curve_public_key(), sec::KeyFileRole::PublicKeyFile,
                       sec::ExistingFilePolicy::Replace);
}

} // namespace pylabhub::utils
