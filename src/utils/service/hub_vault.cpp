/**
 * @file hub_vault.cpp
 * @brief HubVault — encrypted hub secrets store.
 *
 * Crypto layer delegated to vault_crypto.hpp (shared with RoleVault).
 * This file handles only hub-specific payload structure (broker keypair + admin token).
 */
#include "utils/hub_vault.hpp"
#include "plh_platform.hpp"

#include "vault_crypto.hpp"

#include "utils/json_fwd.hpp"
#include <sodium.h>
#include <zmq.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace fs = std::filesystem;
using json   = nlohmann::json;

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
// Helpers
// ============================================================================

namespace
{

constexpr std::size_t kZ85BufSize    = 41; // 40 printable chars + null terminator
constexpr std::size_t kZ85KeyLen     = 40;
constexpr std::size_t kAdminRawBytes = 32; // → 64 hex chars

std::string generate_admin_token()
{
    uint8_t raw[kAdminRawBytes]{};
    randombytes_buf(raw, sizeof(raw));
    char hex[kAdminRawBytes * 2 + 1]{};
    sodium_bin2hex(hex, sizeof(hex), raw, sizeof(raw));
    std::string token(hex, kAdminRawBytes * 2);
    sodium_memzero(raw, sizeof(raw));
    sodium_memzero(hex, sizeof(hex));
    return token;
}

} // anonymous namespace

// ============================================================================
// Constructors / destructor
// ============================================================================

HubVault::HubVault() : pImpl(std::make_unique<Impl>()) {}
HubVault::~HubVault()                       = default;
HubVault::HubVault(HubVault &&) noexcept    = default;
HubVault &HubVault::operator=(HubVault &&) noexcept = default;

// ============================================================================
// HubVault::create
// ============================================================================

HubVault HubVault::create(const fs::path    &hub_dir,
                           const std::string &hub_uid,
                           const std::string &password)
{
    detail::vault_require_sodium();

    // Generate broker CurveZMQ keypair (Z85).
    std::array<char, kZ85BufSize> pub{};
    std::array<char, kZ85BufSize> sec{};
    if (zmq_curve_keypair(pub.data(), sec.data()) != 0)
    {
        throw std::runtime_error("HubVault: zmq_curve_keypair failed");
    }
    const std::string broker_public(pub.data(), kZ85KeyLen);
    const std::string broker_secret(sec.data(), kZ85KeyLen);
    sodium_memzero(sec.data(), sec.size()); // zero secret key buffer after copying to string
    sodium_memzero(pub.data(), pub.size()); // zero public key buffer after copying to string

    // Generate admin token.
    const std::string admin_tok = generate_admin_token();

    // Serialize payload and encrypt.  Vault file lives under
    // `<hub_dir>/vault/` (HEP-CORE-0033 §7 + HubDirectory layout).
    // Ensure the vault directory exists; --init creates it but
    // direct callers (tests, programmatic use) may not have.
    const json payload = {
        {"broker", {{"curve_secret_key", broker_secret}, {"curve_public_key", broker_public}}},
        {"admin",  {{"token", admin_tok}}}
    };
    fs::create_directories(hub_dir / "vault");
    detail::vault_write(hub_dir / "vault" / "hub.vault",
                        payload.dump(), password, hub_uid);

    HubVault v;
    v.pImpl->broker_secret_key = broker_secret;
    v.pImpl->broker_public_key = broker_public;
    v.pImpl->admin_token       = admin_tok;
    return v;
}

// ============================================================================
// HubVault::open
// ============================================================================

HubVault HubVault::open(const fs::path    &hub_dir,
                         const std::string &hub_uid,
                         const std::string &password)
{
    detail::vault_require_sodium();

    // Vault file path: <hub_dir>/vault/hub.vault (HEP §7).
    const std::string plaintext = detail::vault_read(
        hub_dir / "vault" / "hub.vault", password, hub_uid);

    HubVault v;
    try
    {
        const json j = json::parse(plaintext);
        v.pImpl->broker_secret_key =
            j.at("broker").at("curve_secret_key").get<std::string>();
        v.pImpl->broker_public_key =
            j.at("broker").at("curve_public_key").get<std::string>();
        v.pImpl->admin_token = j.at("admin").at("token").get<std::string>();
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
        {
            throw std::runtime_error("HubVault: cannot write hub.pubkey: " +
                                     pubkey_path.string());
        }
        ofs << pImpl->broker_public_key;
        if (!ofs)
        {
            throw std::runtime_error("HubVault: write failed: " + pubkey_path.string());
        }
    }
#if defined(PYLABHUB_PLATFORM_WIN64)
    // Windows: set DACL granting owner full access + everyone read.
    {
        WELL_KNOWN_SID_TYPE everyone_type = WinWorldSid;
        BYTE everyone_sid[SECURITY_MAX_SID_SIZE];
        DWORD sid_size = sizeof(everyone_sid);
        CreateWellKnownSid(everyone_type, nullptr, everyone_sid, &sid_size);

        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            DWORD len = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &len);
            std::vector<uint8_t> buf(len);
            if (GetTokenInformation(token, TokenUser, buf.data(), len, &len))
            {
                auto *user = reinterpret_cast<TOKEN_USER *>(buf.data());

                EXPLICIT_ACCESS_W ea[2]{};
                // Owner: full access
                ea[0].grfAccessPermissions = GENERIC_ALL;
                ea[0].grfAccessMode = SET_ACCESS;
                ea[0].grfInheritance = NO_INHERITANCE;
                ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
                ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);
                // Everyone: read
                ea[1].grfAccessPermissions = GENERIC_READ;
                ea[1].grfAccessMode = SET_ACCESS;
                ea[1].grfInheritance = NO_INHERITANCE;
                ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
                ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone_sid);

                PACL acl = nullptr;
                if (SetEntriesInAclW(2, ea, nullptr, &acl) == ERROR_SUCCESS)
                {
                    SetNamedSecurityInfoW(
                        const_cast<wchar_t *>(pubkey_path.wstring().c_str()), SE_FILE_OBJECT,
                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, acl, nullptr);
                    LocalFree(acl);
                }
            }
            CloseHandle(token);
        }
    }
#else
    fs::permissions(pubkey_path,
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
#endif
}

} // namespace pylabhub::utils
