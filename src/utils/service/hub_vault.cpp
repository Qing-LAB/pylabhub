/**
 * @file hub_vault.cpp
 * @brief HubVault — encrypted hub secrets store.
 *
 * Crypto layer delegated to vault_crypto.hpp (shared with RoleVault).
 * This file handles only hub-specific payload structure (broker keypair + admin token).
 */
#include "utils/hub_vault.hpp"
#include "plh_platform.hpp"
#include "utils/security/curve_keypair.hpp"
#include "utils/security/key_file_acl.hpp"
#include "utils/security/secure_buffer.hpp"
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
    /// HEP-CORE-0040 §175: secrets stored in fixed-size buffers + zeroed
    /// in dtor via `sodium_memzero`.  Replaces the pre-#175
    /// `std::string` members whose destructors are not guaranteed to
    /// zero heap memory.
    std::array<char, 40> broker_secret_z85{};
    std::array<char, 40> broker_public_z85{};
    std::array<char, 64> admin_token_hex{};

    /// KeyStore name the vault's decryption key was filed under by
    /// `create()` / `open()`.  `save()` cites it instead of asking the
    /// caller for the password again.  Not secret — it is a name.
    std::string key_name_;

    /// View sizes (`size()` of each accessor's returned `std::string_view`).
    /// All three secrets are fixed-length in this vault, so the views are
    /// always full-size after a successful `open()` or `create()`.
    static constexpr std::size_t kSecretLen = 40;
    static constexpr std::size_t kPublicLen = 40;
    static constexpr std::size_t kAdminLen = 64;

    /// The `known_roles` allowlist document (HEP-CORE-0035 §4.8).
    /// PUBLIC-key data — not secret — so it is a plain json member with
    /// no zero-on-destruct treatment (unlike the secrets above).  Held
    /// OPAQUELY: the vault never interprets its `{version, roles:[…]}`
    /// schema (that is `KnownRolesStore`'s job).  Defaults to an empty
    /// object = §4.8.4 deny-all bootstrap.
    json known_roles = json::object();

    ~Impl() noexcept
    {
        // Wipe via SMS (HEP-CORE-0043 §2.1 category 1) — the security
        // module owns every sodium primitive.  Cast to uint8_t* is safe
        // (byte-addressable char array).
        namespace sec = pylabhub::utils::security;
        sec::secure().memzero(std::span<std::uint8_t>(
            reinterpret_cast<std::uint8_t *>(broker_secret_z85.data()), broker_secret_z85.size()));
        sec::secure().memzero(std::span<std::uint8_t>(
            reinterpret_cast<std::uint8_t *>(broker_public_z85.data()), broker_public_z85.size()));
        sec::secure().memzero(std::span<std::uint8_t>(
            reinterpret_cast<std::uint8_t *>(admin_token_hex.data()), admin_token_hex.size()));
    }
};

// ============================================================================
// Helpers
// ============================================================================

namespace
{

constexpr std::size_t kAdminRawBytes = 32; // → 64 hex chars

std::string generate_admin_token()
{
    namespace sec = pylabhub::utils::security;
    uint8_t raw[kAdminRawBytes]{};
    sec::secure().random_bytes(raw, sizeof(raw));
    char hex[kAdminRawBytes * 2 + 1]{};
    sec::secure().bin2hex(hex, sizeof(hex), raw, sizeof(raw));
    std::string token(hex, kAdminRawBytes * 2);
    sec::secure().memzero(std::span<std::uint8_t>(raw, sizeof(raw)));
    sec::secure().memzero(
        std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(hex), sizeof(hex)));
    return token;
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
                          const std::string &password, std::string_view key_name)
{
    // Generate broker CurveZMQ keypair (Z85).
    auto kp = pylabhub::utils::security::generate_curve_keypair();
    const std::string broker_public = std::move(kp.public_z85);
    const std::string broker_secret = std::move(kp.secret_z85);

    // Generate admin token.
    const std::string admin_tok = generate_admin_token();

    // Serialize payload and encrypt at the operator-supplied path
    // (HEP-CORE-0033 §7.1 — caller resolved via resolve_keyfile_path).
    // Ensure the parent directory exists; --init creates the
    // canonical `vault/` dir but direct callers (tests, custom
    // deployment locations) may not have.
    // §4.8.4: a freshly created vault has an EMPTY known_roles map —
    // deny-all bootstrap (every REG_REQ rejected until the operator
    // runs `--add-known-role`).  Stored opaquely as an empty object.
    const json payload = {
        {"broker", {{"curve_secret_key", broker_secret}, {"curve_public_key", broker_public}}},
        {"admin", {{"token", admin_tok}}},
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
    detail::vault_add_key_from_password(key_name, password, hub_uid);
    detail::vault_write(vault_path, payload.dump(), key_name);

    HubVault v;
    v.pImpl->key_name_ = std::string(key_name);
    if (broker_secret.size() != HubVault::Impl::kSecretLen ||
        broker_public.size() != HubVault::Impl::kPublicLen ||
        admin_tok.size() != HubVault::Impl::kAdminLen)
    {
        throw std::runtime_error("HubVault::create: generated material has unexpected length");
    }
    std::memcpy(v.pImpl->broker_secret_z85.data(), broker_secret.data(),
                HubVault::Impl::kSecretLen);
    std::memcpy(v.pImpl->broker_public_z85.data(), broker_public.data(),
                HubVault::Impl::kPublicLen);
    std::memcpy(v.pImpl->admin_token_hex.data(), admin_tok.data(), HubVault::Impl::kAdminLen);
    return v;
}

// ============================================================================
// HubVault::open
// ============================================================================

HubVault HubVault::open(const fs::path &vault_path, const std::string &hub_uid,
                        const std::string &password, std::string_view key_name)
{
    // HEP-CORE-0040 §175: decrypt directly into a stack buffer whose
    // destructor zeroes the plaintext when this scope exits.  Sized
    // generously for the small JSON payload (broker keys + admin
    // token + framing); fits comfortably under 4 KiB.
    pylabhub::utils::security::SecureBuffer<4096> json_buf;
    detail::vault_add_key_from_password(key_name, password, hub_uid);
    const std::size_t n = detail::vault_read_secure(vault_path, key_name, json_buf.span());

    HubVault v;
    v.pImpl->key_name_ = std::string(key_name);
    try
    {
        const auto bytes = json_buf.span().first(n);
        const json j = json::parse(reinterpret_cast<const char *>(bytes.data()),
                                   reinterpret_cast<const char *>(bytes.data() + bytes.size()));

        auto copy_into =
            [](auto &dst, std::size_t expected_len, const std::string &src, const char *field)
        {
            if (src.size() != expected_len)
            {
                throw std::runtime_error(std::string("HubVault: ") + field +
                                         " has unexpected length (" + std::to_string(src.size()) +
                                         ", expected " + std::to_string(expected_len) + ")");
            }
            std::memcpy(dst.data(), src.data(), expected_len);
        };

        // Extract via temporary std::strings (json's internal storage
        // is std::string-backed).  The temporaries live only as long
        // as these statements; the json object's own internal copies
        // live until `j` destructs at end of scope (microsecond
        // window of unlocked exposure — bounded because identity
        // bytes live in the process KeyStore (locked memory) from
        // `HubConfig::load_keypair` onward per HEP-CORE-0040 §172).
        copy_into(v.pImpl->broker_secret_z85, HubVault::Impl::kSecretLen,
                  j.at("broker").at("curve_secret_key").get<std::string>(),
                  "broker.curve_secret_key");
        copy_into(v.pImpl->broker_public_z85, HubVault::Impl::kPublicLen,
                  j.at("broker").at("curve_public_key").get<std::string>(),
                  "broker.curve_public_key");
        copy_into(v.pImpl->admin_token_hex, HubVault::Impl::kAdminLen,
                  j.at("admin").at("token").get<std::string>(), "admin.token");

        // known_roles (HEP-CORE-0035 §4.8) — PUBLIC data, extracted as
        // an opaque document.  Absent in vaults created before this
        // field existed → empty object (§4.8.4 deny-all bootstrap).
        // Not zeroed: pubkeys are not secret.
        v.pImpl->known_roles = j.contains("known_roles") ? j.at("known_roles") : json::object();
    }
    catch (const json::exception &e)
    {
        throw std::runtime_error(std::string("HubVault: vault payload invalid: ") + e.what());
    }
    return v;
}

// ============================================================================
// Accessors
// ============================================================================

std::string_view HubVault::broker_curve_secret_key() const noexcept
{
    return std::string_view(pImpl->broker_secret_z85.data(), Impl::kSecretLen);
}

std::string_view HubVault::broker_curve_public_key() const noexcept
{
    return std::string_view(pImpl->broker_public_z85.data(), Impl::kPublicLen);
}

std::string_view HubVault::admin_token() const noexcept
{
    return std::string_view(pImpl->admin_token_hex.data(), Impl::kAdminLen);
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
    // Reconstruct the full payload from the in-memory state.  The
    // keypair + token round-trip unchanged from open(); only
    // known_roles reflects any set_known_roles() call.  The secrets
    // sit briefly inside the payload string during dump()/encrypt —
    // the same bounded exposure create() already has.
    const json payload = {
        {"broker",
         {{"curve_secret_key", std::string(pImpl->broker_secret_z85.data(), Impl::kSecretLen)},
          {"curve_public_key", std::string(pImpl->broker_public_z85.data(), Impl::kPublicLen)}}},
        {"admin", {{"token", std::string(pImpl->admin_token_hex.data(), Impl::kAdminLen)}}},
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
    detail::vault_write(tmp, payload.dump(), pImpl->key_name_);
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
