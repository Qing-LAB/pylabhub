/**
 * @file test_role_vault.cpp
 * @brief Unit tests for RoleVault: create, open, encryption verification.
 *
 * Mirrors the HubVault test pattern. RoleVault is simpler (no admin_token,
 * no publish_public_key) — stores only public_key + secret_key + role_uid.
 *
 * Pure API test — no lifecycle, no workers. Each test gets an isolated temp
 * directory. Argon2id KDF takes ~0.5s per call; timeout set to 120s.
 */
#include "utils/role_vault.hpp"
#include "utils/security/key_file_acl.hpp"
#include "utils/uuid_utils.hpp"

#include "utils/logger.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "binary_lifecycle.h"
#include "utils/security/key_store.hpp"
#include <array>
#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#endif

#include "plh_platform.hpp"

namespace fs = std::filesystem;
using pylabhub::utils::generate_uuid4;
using pylabhub::utils::RoleVault;

namespace
{

constexpr const char *kPassword = "test-role-password-123";
constexpr const char *kWrongPassword = "wrong-password-456";

/// Validate a Z85 key: exactly 40 printable ASCII chars (no space).
bool is_valid_z85_key(std::string_view s)
{
    if (s.size() != 40)
        return false;
    for (char c : s)
        if (c < 0x21 || c > 0x7E)
            return false;
    return true;
}

} // anonymous namespace

// ============================================================================
// Fixture
// ============================================================================

// Pattern 1+ (BinaryLifecycleEnvironment) — the vault deposits its
// decryption key in the process KeyStore, so `SecureSubsystem` must be
// up before any RoleVault call.  This is not a test accommodation: it is
// what production does.  Before the key moved into locked memory the
// vault only used ungated primitives and these tests ran with no
// lifecycle at all, exercising a configuration production never has.
PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule(),
                             pylabhub::utils::security::SecureSubsystem::GetLifecycleModule())

class RoleVaultTest : public ::testing::Test
{
  protected:
    fs::path vault_dir_;
    fs::path vault_path_;
    std::string role_uid_;

    void SetUp() override
    {
        role_uid_ = "prod.test.u" + generate_uuid4().substr(0, 8);
        vault_dir_ =
            fs::temp_directory_path() / ("plh_test_role_vault_" + generate_uuid4().substr(0, 8));
        fs::create_directories(vault_dir_);
        vault_path_ = vault_dir_ / "role.key";
    }

    void TearDown() override
    {
        try
        {
            if (fs::exists(vault_dir_))
                fs::remove_all(vault_dir_);
        }
        catch (...)
        {
        }
    }
};

// ============================================================================
// Creation tests
// ============================================================================

TEST_F(RoleVaultTest, Create_WritesFile)
{
    RoleVault::create(vault_path_, role_uid_, kPassword);
    ASSERT_TRUE(fs::exists(vault_path_)) << "Vault file not created";
    EXPECT_GT(fs::file_size(vault_path_), 40u) << "Vault file suspiciously small";
}

TEST_F(RoleVaultTest, Create_RestrictedPerms)
{
#if !defined(PYLABHUB_PLATFORM_WIN64)
    // Windows has no POSIX file modes; skip permission check.
    RoleVault::create(vault_path_, role_uid_, kPassword);

    // Mode discipline owned by HEP-CORE-0035 §4.6 utility (single
    // source of truth for the verdict matrix; see
    // tests/test_layer2_service/test_key_file_acl.cpp).
    using pylabhub::utils::security::KeyFileRole;
    using pylabhub::utils::security::verify_keyfile_acl;
    const auto v = verify_keyfile_acl(vault_path_, KeyFileRole::VaultFile);
    EXPECT_TRUE(v.ok) << v.diagnostic;
#else
    GTEST_SKIP() << "File permission test not applicable on Windows";
#endif
}

// ── Secret-half checks without the secret leaving locked memory ─────
//
// These tests used to read `RoleVault::secret_key()`, a view into an
// UNLOCKED member of the vault object.  That accessor is going away
// (HEP-CORE-0043 §2.5), and the replacement is the production path:
// deposit the identity into the KeyStore, then use the use-not-export
// accessors.
//
// Comparisons are done on a BLAKE2b fingerprint computed INSIDE the
// `with_seckey_z85` callback, so the secret is never copied into a
// test-local string.  Comparing fingerprints proves the secret halves
// match; it does not require either of them to be handled.
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

bool deposited_seckey_is_valid_z85(std::string_view key_name)
{
    bool ok = false;
    vsec::secure().keys().with_seckey_z85(key_name,
                                          [&](std::string_view sk) { ok = is_valid_z85_key(sk); });
    return ok;
}
} // namespace

TEST_F(RoleVaultTest, Create_ValidZ85Keypair)
{
    RoleVault v = RoleVault::create(vault_path_, role_uid_, kPassword);
    EXPECT_TRUE(is_valid_z85_key(v.public_key()))
        << "public_key is not a valid 40-char Z85 key: '" << v.public_key() << "'";
    v.load_identity_into("rv:create");
    EXPECT_TRUE(deposited_seckey_is_valid_z85("rv:create"))
        << "secret half is not a valid 40-char Z85 key";
}

TEST_F(RoleVaultTest, Create_EmptyPassword)
{
    // Dev-mode: empty password is allowed.
    EXPECT_NO_THROW(RoleVault::create(vault_path_, role_uid_, ""));
    EXPECT_TRUE(fs::exists(vault_path_));
}

// ============================================================================
// Open tests
// ============================================================================

TEST_F(RoleVaultTest, Open_CorrectPassword)
{
    RoleVault created = RoleVault::create(vault_path_, role_uid_, kPassword);
    RoleVault opened = RoleVault::open(vault_path_, role_uid_, kPassword);

    EXPECT_EQ(created.public_key(), opened.public_key());
    created.load_identity_into("rv:created");
    opened.load_identity_into("rv:opened");
    EXPECT_EQ(seckey_fingerprint("rv:created"), seckey_fingerprint("rv:opened"))
        << "reopening the vault yielded a different secret half";
    EXPECT_EQ(created.role_uid(), opened.role_uid());
}

TEST_F(RoleVaultTest, Open_WrongPassword_Throws)
{
    RoleVault::create(vault_path_, role_uid_, kPassword);
    EXPECT_THROW(RoleVault::open(vault_path_, role_uid_, kWrongPassword), std::runtime_error);
}

TEST_F(RoleVaultTest, Open_CorruptedFile_Throws)
{
    RoleVault::create(vault_path_, role_uid_, kPassword);

    // Flip bytes in the ciphertext (after the 24-byte nonce).
    {
        std::fstream f(vault_path_, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekp(30, std::ios::beg);
        const char garbage[] = {'\xDE', '\xAD', '\xBE', '\xEF'};
        f.write(garbage, sizeof(garbage));
    }
#if !defined(PYLABHUB_PLATFORM_WIN64)
    // Restore canonical 0600 vault-file mode via HEP-CORE-0035 §4.6
    // utility (fstream may not preserve them).
    EXPECT_EQ(pylabhub::utils::security::set_keyfile_mode(
                  vault_path_, pylabhub::utils::security::KeyFileRole::VaultFile),
              pylabhub::utils::security::SetModeResult::Applied);
#endif

    EXPECT_THROW(RoleVault::open(vault_path_, role_uid_, kPassword), std::runtime_error);
}

TEST_F(RoleVaultTest, Open_MissingFile_Throws)
{
    EXPECT_THROW(RoleVault::open(vault_path_, role_uid_, kPassword), std::runtime_error);
}

// ============================================================================
// Encryption verification
// ============================================================================

TEST_F(RoleVaultTest, Encrypt_SecretsNotInPlaintext)
{
    RoleVault v = RoleVault::create(vault_path_, role_uid_, kPassword);

    std::ifstream ifs(vault_path_, std::ios::binary);
    const std::string raw_bytes((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());

    // Searching for the PUBLIC key is the sufficient check, and it needs
    // no secret.  Both keys are values in the same JSON payload, so an
    // unencrypted file exposes both — finding neither means the payload
    // is not on disk in the clear.  The public key is not secret, so
    // this test can hold it; the secret-key search this replaces
    // required an accessor that is going away.
    EXPECT_EQ(raw_bytes.find(v.public_key()), std::string::npos)
        << "Public key appears in plaintext in vault file — the payload is NOT encrypted, "
           "which means the secret key is sitting there too";
    EXPECT_EQ(raw_bytes.find(std::string_view{"secret_key"}), std::string::npos)
        << "the JSON field name 'secret_key' is on disk in the clear — the payload was written "
           "unencrypted, so the key beside it is exposed";
}

TEST_F(RoleVaultTest, Encrypt_DifferentUid_DifferentCiphertext)
{
    const std::string uid_a = "prod.aaa.u" + generate_uuid4().substr(0, 8);
    const std::string uid_b = "prod.bbb.u" + generate_uuid4().substr(0, 8);
    const fs::path path_a = vault_dir_ / "a.key";
    const fs::path path_b = vault_dir_ / "b.key";

    RoleVault::create(path_a, uid_a, kPassword);
    RoleVault::create(path_b, uid_b, kPassword);

    // Cross-uid open should fail (different KDF salt).
    EXPECT_THROW(RoleVault::open(path_a, uid_b, kPassword), std::runtime_error);
    EXPECT_THROW(RoleVault::open(path_b, uid_a, kPassword), std::runtime_error);
}

// ============================================================================
// Identity tests
// ============================================================================

TEST_F(RoleVaultTest, RoleUid_Roundtrip)
{
    RoleVault created = RoleVault::create(vault_path_, role_uid_, kPassword);
    EXPECT_EQ(created.role_uid(), role_uid_);

    RoleVault opened = RoleVault::open(vault_path_, role_uid_, kPassword);
    EXPECT_EQ(opened.role_uid(), role_uid_);
}

TEST_F(RoleVaultTest, Create_OverExistingVault_Throws_AtomicNoOverwrite)
{
    // HEP-CORE-0035 §4.6.1: vault write is atomic O_CREAT|O_EXCL.
    // A second create against an existing path MUST throw — silent
    // overwrite would invalidate any hub-side allowlist entry pinned
    // to the old pubkey, stranding the role.  Mutation-sweep against
    // the prior (pre-2026-06-01) contract.
    RoleVault v1 = RoleVault::create(vault_path_, role_uid_, kPassword);
    const std::string sentinel_pk{v1.public_key()};

    // Pin the atomic-layer message — distinct from the operator-
    // friendly config-layer fs::exists() refusal that fires earlier
    // in --keygen.  Both must continue to refuse + cite the contract.
    try
    {
        (void)RoleVault::create(vault_path_, role_uid_, kPassword);
        FAIL() << "Second create against existing role vault must refuse atomically";
    }
    catch (const std::runtime_error &ex)
    {
        const std::string msg = ex.what();
        EXPECT_NE(msg.find("HEP-CORE-0035"), std::string::npos)
            << "atomic-layer message must cite HEP-CORE-0035; got: " << msg;
        EXPECT_NE(msg.find("already exists"), std::string::npos)
            << "atomic-layer message must say 'already exists'; got: " << msg;
    }

    // Original vault content survives — verify by re-opening.
    RoleVault still = RoleVault::open(vault_path_, role_uid_, kPassword);
    EXPECT_EQ(still.public_key(), sentinel_pk)
        << "Failed atomic-no-overwrite create must NOT mutate the existing "
           "role vault — original pubkey should still decrypt";
}

#if !defined(_WIN32) && !defined(_WIN64)
TEST_F(RoleVaultTest, Create_OverSymlinkAtVaultPath_Throws_AtomicNoFollow)
{
    namespace fs = std::filesystem;
    fs::create_directories(vault_path_.parent_path());
    const fs::path target = vault_path_.parent_path() / "attacker_target";
    {
        std::ofstream sink(target);
        sink << "would receive redirected role secret";
    }
    fs::create_symlink(target, vault_path_);
    ASSERT_TRUE(fs::is_symlink(vault_path_));

    EXPECT_THROW(RoleVault::create(vault_path_, role_uid_, kPassword), std::runtime_error)
        << "Create against a symlink at vault_path must refuse atomically";

    EXPECT_TRUE(fs::is_symlink(vault_path_));
    std::ifstream check(target);
    std::string content((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>{});
    EXPECT_EQ(content, "would receive redirected role secret")
        << "symlink target must remain untouched by refused create";
}

TEST_F(RoleVaultTest, Create_VaultFileIsMode0600_AndParentDirIs0700)
{
    const ::mode_t prev_umask = ::umask(0);
    RoleVault v = RoleVault::create(vault_path_, role_uid_, kPassword);
    ::umask(prev_umask);

    namespace fs = std::filesystem;
    const auto file_status = fs::status(vault_path_);
    EXPECT_EQ(static_cast<unsigned>(file_status.permissions()) & 0777, 0600u)
        << "role vault file mode must be 0600 even with umask 0";

    const auto dir_status = fs::status(vault_path_.parent_path());
    EXPECT_EQ(static_cast<unsigned>(dir_status.permissions()) & 0777, 0700u)
        << "role vault parent dir mode must be 0700 even with umask 0";
}
#endif

TEST_F(RoleVaultTest, MoveConstructor_TransfersOwnership)
{
    RoleVault v1 = RoleVault::create(vault_path_, role_uid_, kPassword);
    const std::string pk{v1.public_key()};
    const std::string uid{v1.role_uid()};
    v1.load_identity_into("rv:before_move");
    const auto fp_before = seckey_fingerprint("rv:before_move");

    RoleVault v2(std::move(v1));
    EXPECT_EQ(v2.public_key(), pk);
    EXPECT_EQ(v2.role_uid(), uid);
    v2.load_identity_into("rv:after_move");
    EXPECT_EQ(seckey_fingerprint("rv:after_move"), fp_before)
        << "move did not carry the secret half across";
}

TEST_F(RoleVaultTest, DifferentUids_DifferentKeys)
{
    const std::string uid_a = "cons.x.u" + generate_uuid4().substr(0, 8);
    const std::string uid_b = "cons.y.u" + generate_uuid4().substr(0, 8);
    const fs::path path_a = vault_dir_ / "a.key";
    const fs::path path_b = vault_dir_ / "b.key";

    RoleVault va = RoleVault::create(path_a, uid_a, kPassword);
    RoleVault vb = RoleVault::create(path_b, uid_b, kPassword);

    EXPECT_NE(va.public_key(), vb.public_key())
        << "Two vaults with different UIDs produced the same public key";
}
