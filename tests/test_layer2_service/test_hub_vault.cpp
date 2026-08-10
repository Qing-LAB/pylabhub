/**
 * @file test_hub_vault.cpp
 * @brief Unit tests for HubVault: create, open, publish_public_key.
 *
 * HubVault has no lifecycle dependency — sodium_init() is called internally,
 * and the only external state is the filesystem. Tests run in-process with
 * gtest_main; each test gets an isolated temp directory.
 *
 * Note: Argon2id with OPSLIMIT_INTERACTIVE runs in ~0.5 s per call on reference
 * hardware. Each test that invokes create() or open() will therefore take ~0.5–1 s.
 * The suite timeout is set to 120 s to accommodate this.
 */
#include "plh_platform.hpp"
#include "utils/hub_vault.hpp"
#include "utils/security/key_file_acl.hpp"
#include "utils/uuid_utils.hpp"

#include "utils/logger.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "binary_lifecycle.h"
#include "utils/security/key_store.hpp"
#include <array>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#endif

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <aclapi.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace fs = std::filesystem;

#if defined(PYLABHUB_PLATFORM_WIN64)
/// Check whether a well-known SID has specific access rights on a file.
static bool win32_sid_has_access(const fs::path &path, WELL_KNOWN_SID_TYPE sid_type,
                                 DWORD desired_access)
{
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid);
    if (!CreateWellKnownSid(sid_type, nullptr, sid, &sid_size))
        return false;

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS)
        return false;

    TRUSTEE_W trustee{};
    trustee.TrusteeForm = TRUSTEE_IS_SID;
    trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);

    ACCESS_MASK granted = 0;
    GetEffectiveRightsFromAclW(dacl, &trustee, &granted);
    LocalFree(sd);

    return (granted & desired_access) == desired_access;
}
#endif
using pylabhub::utils::generate_uuid4;
using pylabhub::utils::HubVault;

namespace
{

// Shared test constants
constexpr const char *kPassword = "test-password-123";
constexpr const char *kWrongPassword = "wrong-password-456";

// Validate a Z85 key: exactly 40 printable ASCII chars.
bool is_valid_z85_key(std::string_view s)
{
    if (s.size() != 40)
        return false;
    for (char c : s)
        if (c < 0x21 || c > 0x7E) // printable ASCII, no space
            return false;
    return true;
}

// Validate a 64-char lowercase hex token.
bool is_valid_hex_token(std::string_view s)
{
    if (s.size() != 64)
        return false;
    for (char c : s)
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0)
            return false;
    return true;
}

} // anonymous namespace

// ============================================================================
// Fixture
// ============================================================================

// Pattern 1+ (BinaryLifecycleEnvironment) — the vault deposits its
// decryption key in the process KeyStore, so `SecureSubsystem` must be
// up before any HubVault call.  This is not a test accommodation: it is
// what production does.  Before the key moved into locked memory the
// vault only used ungated primitives and these tests ran with no
// lifecycle at all, exercising a configuration production never has.
PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule(),
                             pylabhub::utils::security::SecureSubsystem::GetLifecycleModule())

class HubVaultTest : public ::testing::Test
{
  protected:
    fs::path hub_dir_;
    fs::path vault_path_; // Canonical default: hub_dir_/vault/hub.vault.
                          // Mirrors what hub_config.cpp computes via
                          // resolve_keyfile_path() at runtime.
    std::string hub_uid_;

    void SetUp() override
    {
        test_tag_ = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        hub_uid_ = generate_uuid4();
        hub_dir_ = fs::temp_directory_path() / ("pylabhub_vault_test_" + hub_uid_.substr(0, 8));
        vault_path_ = hub_dir_ / "vault" / "hub.vault";
        fs::create_directories(hub_dir_);
    }

    /// Unique KeyStore names for this TEST_F, suffixed for call sites
    /// that open more than one vault.
    ///
    /// These have to be per-test because the KeyStore is a PROCESS
    /// singleton shared by every test in this binary, and `add_identity`
    /// refuses to replace an existing name — deliberately, since an
    /// identity being silently overwritten is a security event.  Reusing
    /// one name across tests would make the second create fail with
    /// "already present" instead of testing what it means to test.
    ///
    /// Generated rather than hand-written: 25 call sites needing two
    /// names each is 50 string literals to keep unique by eye, and a
    /// duplicate would surface as a confusing unrelated failure.
    [[nodiscard]] std::string id_(std::string_view suffix = "") const
    {
        return "hv:" + test_tag_ + ":id" + std::string(suffix);
    }
    [[nodiscard]] std::string tok_(std::string_view suffix = "") const
    {
        return "hv:" + test_tag_ + ":tok" + std::string(suffix);
    }

    std::string test_tag_;

    void TearDown() override
    {
        try
        {
            if (fs::exists(hub_dir_))
                fs::remove_all(hub_dir_);
        }
        catch (...)
        {
        }
    }
};

// ============================================================================
// Creation tests
// ============================================================================

TEST_F(HubVaultTest, CreateWritesVaultFile)
{
    HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    const fs::path vault_path = hub_dir_ / "vault" / "hub.vault";
    ASSERT_TRUE(fs::exists(vault_path)) << "hub.vault not created";
    EXPECT_GT(fs::file_size(vault_path), 40u) << "hub.vault suspiciously small";
}

TEST_F(HubVaultTest, CreateVaultFileHasRestrictedPermissions)
{
    HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    const fs::path vault_path = hub_dir_ / "vault" / "hub.vault";
#if defined(PYLABHUB_PLATFORM_WIN64)
    // Windows: verify the DACL denies access to the "Everyone" group.
    // fs::permissions() cannot represent POSIX group/other bits on Windows;
    // the actual security enforcement is via Win32 ACLs.
    EXPECT_FALSE(win32_sid_has_access(vault_path, WinWorldSid, FILE_GENERIC_READ))
        << "Everyone should NOT have read access to hub.vault";
    EXPECT_FALSE(win32_sid_has_access(vault_path, WinWorldSid, FILE_GENERIC_WRITE))
        << "Everyone should NOT have write access to hub.vault";
#else
    // Mode discipline owned by HEP-CORE-0035 §4.6 utility (single
    // source of truth for the verdict matrix; see
    // tests/test_layer2_service/test_key_file_acl.cpp).
    using pylabhub::utils::security::KeyFileRole;
    using pylabhub::utils::security::verify_keyfile_acl;
    const auto v = verify_keyfile_acl(vault_path, KeyFileRole::VaultFile);
    EXPECT_TRUE(v.ok) << v.diagnostic;
#endif
}

// ── Secret-half checks without the secret leaving locked memory ─────
//
// These tests used to read `HubVault::broker_curve_secret_key()`, a view
// into an UNLOCKED member of the vault object.  That accessor is going
// away (HEP-CORE-0043 §2.5); the replacement is the production path —
// deposit the identity into the KeyStore, then use the use-not-export
// accessors.
//
// Comparisons use a BLAKE2b fingerprint computed INSIDE the
// `with_seckey_z85` callback, so the secret is never copied into a
// test-local string.  Equal fingerprints prove the secret halves match
// without either being handled.
namespace
{
namespace vsec = pylabhub::utils::security;

std::array<std::uint8_t, 32> seckey_fingerprint(std::string_view key_name)
{
    std::array<std::uint8_t, 32> h{};
    bool hashed = false;
    vsec::secure().keys().with_seckey_z85(
        key_name, [&](std::string_view sk)
        { hashed = vsec::secure().compute_blake2b(h.data(), sk.data(), sk.size()); });
    EXPECT_TRUE(hashed) << "no secret yielded for KeyStore entry '" << key_name << "'";
    return h;
}

/// BLAKE2b of the raw admin token, computed INSIDE `with_raw_key`.
///
/// There is no `admin_token()` accessor to compare against any more —
/// the token is 32 raw bytes in locked memory and the design keeps it
/// there (HEP-CORE-0043 §2.5.3.2).  A fingerprint answers every question
/// these tests actually ask — "is it the same token?", "did two vaults
/// mint different ones?" — without the token being handled.
std::array<std::uint8_t, 32> token_fingerprint(std::string_view key_name)
{
    std::array<std::uint8_t, 32> h{};
    bool hashed = false;
    vsec::secure().keys().with_raw_key(
        key_name,
        [&](std::span<const std::byte> raw)
        { hashed = vsec::secure().compute_blake2b(h.data(), raw.data(), raw.size()); });
    EXPECT_TRUE(hashed) << "no raw token yielded for KeyStore entry '" << key_name << "'";
    return h;
}

/// Length of the raw secret behind `key_name`, or 0 if absent.
std::size_t raw_key_size(std::string_view key_name)
{
    std::size_t n = 0;
    vsec::secure().keys().with_raw_key(key_name,
                                       [&](std::span<const std::byte> raw) { n = raw.size(); });
    return n;
}

bool deposited_seckey_is_valid_z85(std::string_view key_name)
{
    bool ok = false;
    vsec::secure().keys().with_seckey_z85(key_name,
                                          [&](std::string_view sk) { ok = is_valid_z85_key(sk); });
    return ok;
}
} // namespace

TEST_F(HubVaultTest, CreateReturnsValidZ85Keypair)
{
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    EXPECT_TRUE(is_valid_z85_key(v.broker_curve_public_key()))
        << "broker_curve_public_key is not a valid 40-char Z85 key: '"
        << v.broker_curve_public_key() << "'";
    // No hand-off step: `create` deposited the identity itself.
    EXPECT_TRUE(deposited_seckey_is_valid_z85(id_()))
        << "broker_curve_secret_key is not a valid 40-char Z85 key";
}

TEST_F(HubVaultTest, CreateMintsA32ByteAdminTokenIntoLockedMemory)
{
    // RENAMED, because what it checks changed.  The token used to be a
    // 64-char hex `std::string` handed back by `admin_token()`; under
    // HEP-CORE-0035 §4.6.6 it is 32 RAW bytes deposited straight into
    // the key store, and hex is only a wire encoding.  Asserting "is it
    // 64 hex chars" would now be asserting the wrong shape.
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    EXPECT_EQ(raw_key_size(tok_()), 32u)
        << "admin token must be 32 raw bytes in the key store";

    // And it must be usable as a credential: the verification path
    // accepts the real token and rejects a near-miss.  This is the
    // assertion that proves the whole named-token scheme works
    // end-to-end, and it needs no accessor — the hex form is built here
    // only to present it, exactly as an operator would.
    std::string presented;
    vsec::secure().keys().with_raw_key(
        tok_(),
        [&](std::span<const std::byte> raw)
        {
            presented.resize(raw.size() * 2 + 1);
            vsec::secure().bin2hex(presented.data(), presented.size(),
                                   reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size());
            presented.resize(raw.size() * 2);
        });
    ASSERT_EQ(presented.size(), 64u);
    EXPECT_TRUE(vsec::secure().keys().raw_key_matches_hex(tok_(), presented))
        << "the correct token was rejected";

    std::string wrong = presented;
    wrong[0] = (wrong[0] == 'a') ? 'b' : 'a';
    EXPECT_FALSE(vsec::secure().keys().raw_key_matches_hex(tok_(), wrong))
        << "a token differing in one character was accepted";
    EXPECT_FALSE(vsec::secure().keys().raw_key_matches_hex(tok_(), presented.substr(0, 62)))
        << "a truncated token was accepted";
    EXPECT_FALSE(vsec::secure().keys().raw_key_matches_hex("hv:no.such.name", presented))
        << "an absent key name accepted a token";
}

TEST_F(HubVaultTest, EmptyPasswordCreatesVaultSuccessfully)
{
    // Dev-mode: empty password is allowed (weak but functional).
    EXPECT_NO_THROW(HubVault::create(vault_path_, hub_uid_, "", id_(), tok_()));
    EXPECT_TRUE(fs::exists(hub_dir_ / "vault" / "hub.vault"));
}

TEST_F(HubVaultTest, TwoCreatesProduceDifferentKeypairs)
{
    // Entropy check: two vaults created with the same password must differ.
    const std::string uid_a = generate_uuid4();
    const std::string uid_b = generate_uuid4();
    const fs::path dir_a = hub_dir_ / "a";
    const fs::path dir_b = hub_dir_ / "b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);

    HubVault va = HubVault::create(dir_a / "vault" / "hub.vault", uid_a, kPassword, id_("a"), tok_("a"));
    HubVault vb = HubVault::create(dir_b / "vault" / "hub.vault", uid_b, kPassword, id_("b"), tok_("b"));

    EXPECT_NE(va.broker_curve_public_key(), vb.broker_curve_public_key())
        << "Two vaults produced the same public key — RNG failure?";
    EXPECT_NE(token_fingerprint(tok_("a")), token_fingerprint(tok_("b")))
        << "Two vaults produced the same admin token — RNG failure?";
}

// ============================================================================
// Open tests
// ============================================================================

TEST_F(HubVaultTest, OpenWithCorrectPasswordReturnsMatchingSecrets)
{
    HubVault created = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
    HubVault opened = HubVault::open(vault_path_, hub_uid_, kPassword, id_("o"), tok_("o"));

    EXPECT_EQ(created.broker_curve_public_key(), opened.broker_curve_public_key());
    EXPECT_EQ(seckey_fingerprint(id_()), seckey_fingerprint(id_("o")))
        << "reopening the vault yielded a different broker secret half";
    EXPECT_EQ(token_fingerprint(tok_()), token_fingerprint(tok_("o")))
        << "reopening the vault yielded a different admin token";
}

TEST_F(HubVaultTest, OpenWithWrongPasswordThrows)
{
    HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    EXPECT_THROW(HubVault::open(vault_path_, hub_uid_, kWrongPassword, id_("o"), tok_("o")), std::runtime_error);
}

TEST_F(HubVaultTest, OpenCorruptedVaultThrows)
{
    HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    // Flip bytes in the middle of the ciphertext (after the 24-byte nonce).
    const fs::path vault_path = hub_dir_ / "vault" / "hub.vault";
    {
        std::fstream f(vault_path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekp(30, std::ios::beg); // inside the MAC-protected ciphertext
        const char garbage[] = {'\xDE', '\xAD', '\xBE', '\xEF'};
        f.write(garbage, sizeof(garbage));
    }
    // Restore canonical 0600 vault-file mode via HEP-CORE-0035 §4.6
    // utility (fstream may not preserve them).
    EXPECT_EQ(pylabhub::utils::security::set_keyfile_mode(
                  vault_path, pylabhub::utils::security::KeyFileRole::VaultFile),
              pylabhub::utils::security::SetModeResult::Applied);

    EXPECT_THROW(HubVault::open(vault_path_, hub_uid_, kPassword, id_("o"), tok_("o")), std::runtime_error);
}

TEST_F(HubVaultTest, OpenMissingVaultThrows)
{
    // No create() call — vault file does not exist.
    EXPECT_THROW(HubVault::open(vault_path_, hub_uid_, kPassword, id_("o"), tok_("o")), std::runtime_error);
}

TEST_F(HubVaultTest, OpenTruncatedVaultThrows)
{
    // Companion to OpenCorruptedVaultThrows: verify that a file truncated
    // below the minimum valid vault size (24-byte nonce + MAC-protected
    // ciphertext + footer) is rejected rather than silently decrypted to
    // garbage.  libsodium's secretbox_open fails MAC verification on any
    // truncation, but we pin the behaviour here to catch a regression
    // that added a "partial-read is fine" path.
    HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    const fs::path vault_path = hub_dir_ / "vault" / "hub.vault";
    const auto original_size = fs::file_size(vault_path);
    ASSERT_GT(original_size, 0u);

    // Chop the file to 16 bytes (shorter than even the 24-byte nonce
    // header — open() must fail before touching the decrypt path).
    fs::resize_file(vault_path, 16);
    // Restore canonical 0600 vault-file mode via HEP-CORE-0035 §4.6
    // utility (resize_file may not preserve them).
    EXPECT_EQ(pylabhub::utils::security::set_keyfile_mode(
                  vault_path, pylabhub::utils::security::KeyFileRole::VaultFile),
              pylabhub::utils::security::SetModeResult::Applied);
    ASSERT_EQ(fs::file_size(vault_path), 16u);

    EXPECT_THROW(HubVault::open(vault_path_, hub_uid_, kPassword, id_("o"), tok_("o")), std::runtime_error);
}

// ============================================================================
// Encryption verification tests
// ============================================================================

TEST_F(HubVaultTest, VaultFileDoesNotContainPlaintextSecrets)
{
    // The vault must actually encrypt its payload — raw bytes in the file should
    // not contain the Z85 keys or the admin token as printable substrings.
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    std::ifstream ifs(hub_dir_ / "vault" / "hub.vault", std::ios::binary);
    const std::string raw_bytes((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());

    // Scope, stated precisely: these searches prove the payload was
    // ENCRYPTED AT ALL.  They cannot prove a secret is absent from the
    // METADATA, because the metadata lives inside the ciphertext — a
    // secret smuggled into the JSON would be encrypted along with
    // everything else and every search here would still pass.  That was
    // mutation-verified on the role vault (2026-08-10) and holds
    // identically here.  What pins the secret out of the metadata is the
    // format itself: the secret section is raw bytes at a fixed offset
    // and no code path writes it into the document.
    EXPECT_EQ(raw_bytes.find(v.broker_curve_public_key()), std::string::npos)
        << "Broker public key appears in plaintext in hub.vault — the payload is NOT encrypted, "
           "which means the broker secret key is sitting there too";
    EXPECT_EQ(raw_bytes.find(std::string_view{"curve_public_key"}), std::string::npos)
        << "a metadata field name is on disk in the clear — the payload was written unencrypted";

    // The real secrets, checked without either being copied out.
    bool checked_seckey = false;
    vsec::secure().keys().with_seckey(id_(),
                                      [&](std::string_view raw)
                                      {
                                          checked_seckey = true;
                                          EXPECT_EQ(raw_bytes.find(raw), std::string::npos)
                                              << "the raw broker secret key is in hub.vault";
                                      });
    EXPECT_TRUE(checked_seckey) << "no broker secret yielded — the check never ran";

    bool checked_token = false;
    vsec::secure().keys().with_raw_key(
        tok_(),
        [&](std::span<const std::byte> raw)
        {
            checked_token = true;
            EXPECT_EQ(raw_bytes.find(std::string_view(reinterpret_cast<const char *>(raw.data()),
                                                      raw.size())),
                      std::string::npos)
                << "the raw admin token is in hub.vault — encryption is not working";
        });
    EXPECT_TRUE(checked_token) << "no admin token yielded — the check never ran";

    // The header IS cleartext by design (VF-1).
    ASSERT_GE(raw_bytes.size(), 8u);
    EXPECT_EQ(raw_bytes.compare(0, 8, "PLHVAULT"), 0)
        << "vault file must start with the cleartext magic (HEP-CORE-0035 §4.6.6)";
}

TEST_F(HubVaultTest, EncryptDecryptRoundTrip)
{
    // Full roundtrip: the secrets written by create() must come back unchanged
    // after open(). This verifies the encrypt → file → decrypt pipeline
    // end-to-end with known values.
    HubVault created = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    const std::string expected_pubkey{created.broker_curve_public_key()};
    const auto expected_fp = seckey_fingerprint(id_());
    const auto expected_token_fp = token_fingerprint(tok_());

    // Simulate a new process opening the vault (discard the in-memory object).
    HubVault reopened = HubVault::open(vault_path_, hub_uid_, kPassword, id_("o"), tok_("o"));

    EXPECT_EQ(reopened.broker_curve_public_key(), expected_pubkey)
        << "Public key changed after encrypt/decrypt roundtrip";
    EXPECT_EQ(seckey_fingerprint(id_("o")), expected_fp)
        << "Secret key changed after encrypt/decrypt roundtrip";
    EXPECT_EQ(token_fingerprint(tok_("o")), expected_token_fp)
        << "Admin token changed after encrypt/decrypt roundtrip";
}

TEST_F(HubVaultTest, DifferentHubUidProducesDifferentCiphertext)
{
    // The KDF salt is derived from hub_uid, so the same password + different
    // hub_uid must produce a different encryption key — and thus a different
    // vault file that cannot be decrypted by swapping hub_uids.
    const std::string uid_a = generate_uuid4();
    const std::string uid_b = generate_uuid4();
    const fs::path dir_a = hub_dir_ / "a";
    const fs::path dir_b = hub_dir_ / "b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);

    HubVault::create(dir_a / "vault" / "hub.vault", uid_a, kPassword, id_("a"), tok_("a"));
    HubVault::create(dir_b / "vault" / "hub.vault", uid_b, kPassword, id_("b"), tok_("b"));

    // Opening vault A with uid_b (wrong salt) must fail.
    EXPECT_THROW(HubVault::open(dir_a / "vault" / "hub.vault", uid_b, kPassword, id_("xa"), tok_("xa")),
                 std::runtime_error)
        << "Cross-uid open should fail (wrong KDF salt)";

    // Opening vault B with uid_a must also fail.
    EXPECT_THROW(HubVault::open(dir_b / "vault" / "hub.vault", uid_a, kPassword, id_("xb"), tok_("xb")),
                 std::runtime_error)
        << "Cross-uid open should fail (wrong KDF salt)";
}

// ============================================================================
// known_roles document (HEP-CORE-0035 §4.8)
// ============================================================================
//
// The encrypted vault is the authoritative home of the ZAP allowlist.  These
// tests pin the vault's half of that contract — bootstrap state, in-memory
// vs. on-disk mutation, and survival across a save/reopen cycle — at the
// vault seam, where a vault regression cannot be confused with a CLI one.
// The document's SCHEMA belongs to `KnownRolesStore`; the vault carries it
// opaquely, so these tests deliberately assert on bytes in and bytes out.

namespace
{

// A roster in the shape `KnownRolesStore::to_json` emits (known_roles.hpp
// file-format block).  The vault never parses it — a faithful shape is used
// so the test carries what production actually stores, not a placeholder.
nlohmann::json sample_roster()
{
    return nlohmann::json{
        {"version", 1},
        {"roles", nlohmann::json::array({nlohmann::json{
                      {"name", "lab.daq.sensor1"},
                      {"uid", "prod.sensor.uid12345678"},
                      {"role", "producer"},
                      {"pubkey_z85", "rq:rZbW}gcC-<hV$4ZhF+t)MvA0MMk?e^kD^0BvT"}}})}};
}

} // namespace

TEST_F(HubVaultTest, FreshVault_HasEmptyKnownRoles_DenyAllBootstrap)
{
    // §4.8.4: a newly created vault admits nobody.  If create() ever seeded a
    // non-empty roster, a hub would boot already trusting keys the operator
    // never added.
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());

    EXPECT_TRUE(v.known_roles().is_object())
        << "known_roles must be a JSON object on a fresh vault";
    EXPECT_TRUE(v.known_roles().empty())
        << "a fresh vault must admit no roles (§4.8.4 deny-all bootstrap)";

    // The same must hold after a round trip through the file — the bootstrap
    // state is what a hub actually reads on its first start, not merely what
    // create() returned in memory.
    HubVault reopened = HubVault::open(vault_path_, hub_uid_, kPassword, id_("o"), tok_("o"));
    EXPECT_TRUE(reopened.known_roles().empty())
        << "deny-all bootstrap did not survive create → open";
}

TEST_F(HubVaultTest, SetKnownRoles_WithoutSave_DoesNotReachDisk)
{
    // set_known_roles() is documented as in-memory only.  Pinning it matters
    // because the CLI flow is open → set → save: if set() silently persisted,
    // a command that failed validation after mutating would still have
    // changed the allowlist on disk.
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
    v.set_known_roles(sample_roster());

    EXPECT_FALSE(v.known_roles().empty()) << "set_known_roles did not update memory";

    HubVault reopened = HubVault::open(vault_path_, hub_uid_, kPassword, id_("o"), tok_("o"));
    EXPECT_TRUE(reopened.known_roles().empty())
        << "set_known_roles reached disk without save() — a mutation that was "
           "never committed is now live in the allowlist";
}

TEST_F(HubVaultTest, KnownRoles_SurvivesSaveAndReopen)
{
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
    const nlohmann::json roster = sample_roster();
    v.set_known_roles(roster);
    v.save(vault_path_);

    // Simulate the hub starting in a new process against the saved vault.
    HubVault reopened = HubVault::open(vault_path_, hub_uid_, kPassword, id_("o"), tok_("o"));
    EXPECT_EQ(reopened.known_roles(), roster)
        << "the roster came back different from the one saved";
}

TEST_F(HubVaultTest, Save_PreservesKeypairAndAdminToken)
{
    // save() rewrites the whole payload to persist a known_roles change.  The
    // keypair and token must ride through untouched — regenerating them would
    // silently invalidate every role's pinned server key and the admin's
    // token on an unrelated allowlist edit.
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
    const std::string expected_pubkey{v.broker_curve_public_key()};
    const auto expected_fp = seckey_fingerprint(id_());
    const auto expected_token_fp = token_fingerprint(tok_());

    v.set_known_roles(sample_roster());
    v.save(vault_path_);

    HubVault reopened = HubVault::open(vault_path_, hub_uid_, kPassword, id_("o"), tok_("o"));
    EXPECT_EQ(reopened.broker_curve_public_key(), expected_pubkey)
        << "save() changed the broker public key";
    EXPECT_EQ(seckey_fingerprint(id_("o")), expected_fp)
        << "save() changed the broker secret key";
    EXPECT_EQ(token_fingerprint(tok_("o")), expected_token_fp)
        << "save() changed the admin token";
}

TEST_F(HubVaultTest, KnownRoles_IsStoredInsideTheEncryptedPayload)
{
    // §4.8 makes the ENCRYPTED vault authoritative for the allowlist.  The
    // preceding round-trip tests would all still pass if the roster were
    // written beside the vault in plaintext, so this is the test that pins
    // WHERE it lives: after save(), the roster's contents must not be
    // readable in the vault file, and no plaintext sidecar may appear.
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
    v.set_known_roles(sample_roster());
    v.save(vault_path_);

    std::ifstream ifs(vault_path_, std::ios::binary);
    const std::string raw_bytes((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());

    EXPECT_EQ(raw_bytes.find("prod.sensor.uid12345678"), std::string::npos)
        << "a role uid appears in plaintext in hub.vault — the allowlist is not "
           "inside the encrypted payload";
    EXPECT_EQ(raw_bytes.find("lab.daq.sensor1"), std::string::npos)
        << "a role name appears in plaintext in hub.vault — the allowlist is not "
           "inside the encrypted payload";

    for (const auto &entry : fs::directory_iterator(vault_path_.parent_path()))
    {
        EXPECT_EQ(entry.path().filename(), vault_path_.filename())
            << "save() wrote a second file beside the vault; the encrypted vault "
               "is the only sanctioned home for the allowlist (§4.8)";
    }
}

// ============================================================================
// Atomic vault-create discipline (HEP-CORE-0035 §4.6.1)
// ============================================================================

TEST_F(HubVaultTest, Create_OverExistingVault_Throws_AtomicNoOverwrite)
{
    // HEP-CORE-0035 §4.6.1: vault write is atomic O_CREAT|O_EXCL.
    // A second create against an existing path MUST throw — silent
    // overwrite would destroy the operator's existing keypair AND
    // admin token, invalidating any federation peer that pinned the
    // old pubkey.  Mutation-sweep against the prior (pre-2026-06-01)
    // contract which allowed silent overwrite.
    HubVault v1 = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
    const std::string pk1{v1.broker_curve_public_key()};
    const std::string sentinel_pk = pk1;

    // Pin the atomic-layer message — this is the kernel-enforced
    // refusal at write_secure_file's `open(O_EXCL)`, distinct from
    // the operator-friendly fs::exists() pre-check that fires in
    // the config-layer keygen flow.  Both layers must continue to
    // refuse; both messages must continue to reference the contract.
    try
    {
        (void)HubVault::create(vault_path_, hub_uid_, kPassword, id_("2"), tok_("2"));
        FAIL() << "Second create against existing vault must refuse atomically";
    }
    catch (const std::runtime_error &ex)
    {
        const std::string msg = ex.what();
        EXPECT_NE(msg.find("HEP-CORE-0035"), std::string::npos)
            << "atomic-layer message must cite HEP-CORE-0035; got: " << msg;
        EXPECT_NE(msg.find("already exists"), std::string::npos)
            << "atomic-layer message must say 'already exists'; got: " << msg;
    }

    // Original vault content survives — the failed create did not
    // even open the file (O_EXCL refused before any write).  Pubkey
    // remains accessible by opening with the original password.
    HubVault still = HubVault::open(vault_path_, hub_uid_, kPassword, id_("r"), tok_("r"));
    EXPECT_EQ(still.broker_curve_public_key(), sentinel_pk)
        << "Failed atomic-no-overwrite create must NOT mutate the existing "
           "vault — original pubkey should still decrypt";
}

#if !defined(_WIN32) && !defined(_WIN64)
TEST_F(HubVaultTest, Create_OverSymlinkAtVaultPath_Throws_AtomicNoFollow)
{
    // HEP-CORE-0035 §4.6.1: vault write is atomic O_NOFOLLOW.
    // If the operator's vault path is replaced by a symlink (attacker
    // controls the parent dir momentarily), --keygen MUST refuse —
    // following the symlink would write secret material to an
    // attacker-controlled target.  Pin the kernel-enforced refusal.
    namespace fs = std::filesystem;
    fs::create_directories(vault_path_.parent_path());
    const fs::path target = vault_path_.parent_path() / "attacker_target";
    {
        std::ofstream sink(target);
        sink << "would receive redirected secret";
    }
    fs::create_symlink(target, vault_path_);
    ASSERT_TRUE(fs::is_symlink(vault_path_));

    EXPECT_THROW(HubVault::create(vault_path_, hub_uid_, kPassword, id_("s"), tok_("s")),
                 std::runtime_error)
        << "Create against a symlink at vault_path must refuse atomically";

    // Symlink and target both untouched — the refusal happened at
    // open(2) with EEXIST/ELOOP before any write attempt.
    EXPECT_TRUE(fs::is_symlink(vault_path_));
    std::ifstream check(target);
    std::string content((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>{});
    EXPECT_EQ(content, "would receive redirected secret")
        << "symlink target must remain untouched by refused create";
}

TEST_F(HubVaultTest, Create_VaultFileIsMode0600_AndParentDirIs0700)
{
    // HEP-CORE-0035 §4.6.1: vault file 0600 atomic at create; parent
    // dir 0700 enforced after fs::create_directories.  Pin both
    // ALONG WITH a sentinel `umask(0)` to prove the modes are NOT
    // dependent on the process umask.
    const ::mode_t prev_umask = ::umask(0);
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
    ::umask(prev_umask);

    namespace fs = std::filesystem;
    const auto file_status = fs::status(vault_path_);
    EXPECT_EQ(static_cast<unsigned>(file_status.permissions()) & 0777, 0600u)
        << "vault file mode must be 0600 even with umask 0 — atomic-at-create"
           " + fchmod normalize";

    const auto dir_status = fs::status(vault_path_.parent_path());
    EXPECT_EQ(static_cast<unsigned>(dir_status.permissions()) & 0777, 0700u)
        << "vault parent dir mode must be 0700 even with umask 0 — "
           "set_keyfile_mode(VaultDir) enforces it post-create";
}
#endif

TEST_F(HubVaultTest, MoveConstructor_TransfersMetadata_SecretsAreUnaffected)
{
    // CONTRACT CHANGED, deliberately — same reasoning as the RoleVault
    // twin.  The vault no longer owns either secret, so "move carried
    // the secret across" is not a property it can have.  What replaced
    // it: the secrets' lifetime is the KEY STORE's, and neither moving
    // nor destroying the vault object disturbs them.
    const auto fps = [&]
    {
        HubVault v1 = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
        const std::string pk{v1.broker_curve_public_key()};
        const auto id_fp = seckey_fingerprint(id_());
        const auto tok_fp = token_fingerprint(tok_());

        HubVault v2(std::move(v1));
        EXPECT_EQ(v2.broker_curve_public_key(), pk) << "move lost the broker public key";
        EXPECT_EQ(seckey_fingerprint(id_()), id_fp)
            << "moving the vault object disturbed the deposited broker key";
        EXPECT_EQ(token_fingerprint(tok_()), tok_fp)
            << "moving the vault object disturbed the deposited admin token";
        return std::pair{id_fp, tok_fp};
    }();

    EXPECT_TRUE(vsec::secure().keys().has(id_()))
        << "destroying the vault removed the broker identity";
    EXPECT_TRUE(vsec::secure().keys().has(tok_()))
        << "destroying the vault removed the admin token";
    EXPECT_EQ(seckey_fingerprint(id_()), fps.first);
    EXPECT_EQ(token_fingerprint(tok_()), fps.second);
}

// ============================================================================
// publish_public_key tests
// ============================================================================

TEST_F(HubVaultTest, PublishPublicKeyWritesCorrectContent)
{
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
    v.publish_public_key(hub_dir_);

    const fs::path pubkey_path = hub_dir_ / "hub.pubkey";
    ASSERT_TRUE(fs::exists(pubkey_path)) << "hub.pubkey not written";

    std::ifstream ifs(pubkey_path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, v.broker_curve_public_key())
        << "hub.pubkey content does not match vault public key";
}

TEST_F(HubVaultTest, PublishPublicKeyHasWorldReadablePermissions)
{
    HubVault v = HubVault::create(vault_path_, hub_uid_, kPassword, id_(), tok_());
    v.publish_public_key(hub_dir_);

    const fs::path pubkey_path = hub_dir_ / "hub.pubkey";
#if defined(PYLABHUB_PLATFORM_WIN64)
    // Windows: verify DACL grants Everyone read but NOT write.
    EXPECT_TRUE(win32_sid_has_access(pubkey_path, WinWorldSid, FILE_GENERIC_READ))
        << "Everyone should have read access to hub.pubkey";
    EXPECT_FALSE(win32_sid_has_access(pubkey_path, WinWorldSid, FILE_GENERIC_WRITE))
        << "Everyone should NOT have write access to hub.pubkey";
#else
    const fs::perms p = fs::status(pubkey_path).permissions();
    // Must be 0644: owner rw, group r, others r.
    EXPECT_NE(p & fs::perms::owner_read, fs::perms::none) << "owner_read should be set";
    EXPECT_NE(p & fs::perms::owner_write, fs::perms::none) << "owner_write should be set";
    EXPECT_NE(p & fs::perms::group_read, fs::perms::none) << "group_read should be set";
    EXPECT_NE(p & fs::perms::others_read, fs::perms::none) << "others_read should be set";
    EXPECT_EQ(p & fs::perms::others_write, fs::perms::none) << "others_write should be off";
#endif
}
