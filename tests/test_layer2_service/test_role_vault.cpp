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
#include <nlohmann/json.hpp>

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
    RoleVault::create(vault_path_, role_uid_, kPassword, "rv:writes_file");
    ASSERT_TRUE(fs::exists(vault_path_)) << "Vault file not created";
    EXPECT_GT(fs::file_size(vault_path_), 40u) << "Vault file suspiciously small";
}

TEST_F(RoleVaultTest, Create_RestrictedPerms)
{
#if !defined(PYLABHUB_PLATFORM_WIN64)
    // Windows has no POSIX file modes; skip permission check.
    RoleVault::create(vault_path_, role_uid_, kPassword, "rv:perms");

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
    RoleVault v = RoleVault::create(vault_path_, role_uid_, kPassword, "rv:create");
    EXPECT_TRUE(is_valid_z85_key(v.public_key()))
        << "public_key is not a valid 40-char Z85 key: '" << v.public_key() << "'";
    // No hand-off step: `create` deposited the identity itself.
    EXPECT_TRUE(deposited_seckey_is_valid_z85("rv:create"))
        << "secret half is not a valid 40-char Z85 key";
}

TEST_F(RoleVaultTest, Create_EmptyPassword)
{
    // Dev-mode: empty password is allowed.
    EXPECT_NO_THROW(RoleVault::create(vault_path_, role_uid_, "", "rv:empty_pw"));
    EXPECT_TRUE(fs::exists(vault_path_));
}

// ============================================================================
// Open tests
// ============================================================================

TEST_F(RoleVaultTest, Open_CorrectPassword)
{
    RoleVault created = RoleVault::create(vault_path_, role_uid_, kPassword, "rv:created");
    RoleVault opened = RoleVault::open(vault_path_, role_uid_, kPassword, "rv:opened");

    EXPECT_EQ(created.public_key(), opened.public_key());
    EXPECT_EQ(seckey_fingerprint("rv:created"), seckey_fingerprint("rv:opened"))
        << "reopening the vault yielded a different secret half";
    EXPECT_EQ(created.role_uid(), opened.role_uid());
}

TEST_F(RoleVaultTest, Open_WrongPassword_Throws)
{
    RoleVault::create(vault_path_, role_uid_, kPassword, "rv:wrong_pw");
    EXPECT_THROW(RoleVault::open(vault_path_, role_uid_, kWrongPassword, "rv:wrong_pw_open"),
                 std::runtime_error);
}

TEST_F(RoleVaultTest, Open_CorruptedFile_Throws)
{
    RoleVault::create(vault_path_, role_uid_, kPassword, "rv:corrupt");

    // Flip bytes inside the ciphertext.  Offset 30 lands past the
    // 12-byte header and inside the 24-byte nonce, so this now also
    // exercises nonce corruption — still a tag failure, still refused.
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

    EXPECT_THROW(RoleVault::open(vault_path_, role_uid_, kPassword, "rv:corrupt_open"),
                 std::runtime_error);
}

TEST_F(RoleVaultTest, Open_MissingFile_Throws)
{
    EXPECT_THROW(RoleVault::open(vault_path_, role_uid_, kPassword, "rv:missing"),
                 std::runtime_error);
}

// ============================================================================
// Encryption verification
// ============================================================================

TEST_F(RoleVaultTest, Encrypt_SecretsNotInPlaintext)
{
    RoleVault v = RoleVault::create(vault_path_, role_uid_, kPassword, "rv:plaintext");

    std::ifstream ifs(vault_path_, std::ios::binary);
    const std::string raw_bytes((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());

    // The public key is not secret, so the test may hold it.  Finding it
    // would mean the metadata is on disk in the clear.
    EXPECT_EQ(raw_bytes.find(v.public_key()), std::string::npos)
        << "Public key appears in plaintext in vault file — the payload is NOT encrypted";
    EXPECT_EQ(raw_bytes.find(std::string_view{"role_uid"}), std::string::npos)
        << "the metadata field name 'role_uid' is on disk in the clear — the payload was written "
           "unencrypted";

    // The actual secret bytes must not appear in the file, raw or Z85.
    //
    // Scope, stated precisely because it is narrower than it looks:
    // these two searches prove the payload was ENCRYPTED AT ALL.  They
    // cannot prove the secret is absent from the metadata, because the
    // metadata lives inside the ciphertext — a secret smuggled into the
    // JSON would be encrypted along with everything else and these
    // searches would still pass.  That is not a guess: putting the Z85
    // secret back into the metadata document was mutation-tested on
    // 2026-08-10 and both searches passed.  The size assertion below is
    // what actually pins VF-6.
    bool checked_raw = false;
    vsec::secure().keys().with_seckey("rv:plaintext",
                                      [&](std::string_view raw)
                                      {
                                          checked_raw = true;
                                          EXPECT_EQ(raw_bytes.find(raw), std::string::npos)
                                              << "the raw secret key is present in the vault file";
                                      });
    EXPECT_TRUE(checked_raw) << "no raw secret yielded — the check above never ran";

    bool checked_z85 = false;
    vsec::secure().keys().with_seckey_z85("rv:plaintext",
                                          [&](std::string_view z85)
                                          {
                                              checked_z85 = true;
                                              EXPECT_EQ(raw_bytes.find(z85), std::string::npos)
                                                  << "the Z85-encoded secret key is present in "
                                                     "the vault file";
                                          });
    EXPECT_TRUE(checked_z85) << "no Z85 secret yielded — the check above never ran";

    // The header IS cleartext by design (VF-1) — a reader must act on it
    // before it has a key.  Assert it is there, so that "nothing is
    // readable" never gets over-tightened into breaking the format.
    ASSERT_GE(raw_bytes.size(), 8u);
    EXPECT_EQ(raw_bytes.compare(0, 8, "PLHVAULT"), 0)
        << "vault file must start with the cleartext magic (HEP-CORE-0035 §4.6.6)";

    // VF-6, pinned by the one thing an outsider CAN measure: the exact
    // file length.  The format is fully determined — header 12, nonce
    // 24, secret section 32, metadata, tag 16 — so the total fixes the
    // metadata byte for byte.  Build the metadata the DESIGN specifies
    // (§4.6.6: role_uid and public_key, nothing else) and require the
    // file to be exactly that long.  Any additional field moves the
    // number, whether or not it happens to be a secret.
    const nlohmann::json expected_metadata = {{"role_uid", role_uid_},
                                              {"public_key", std::string{v.public_key()}}};
    const std::size_t expected_size = 12U + 24U + 32U + expected_metadata.dump().size() + 16U;
    EXPECT_EQ(raw_bytes.size(), expected_size)
        << "vault file is not the exact size the format specifies — the metadata carries "
           "something beyond role_uid and public_key (a secret smuggled back into the document "
           "looks exactly like this)";
}

TEST_F(RoleVaultTest, Encrypt_DifferentUid_DifferentCiphertext)
{
    const std::string uid_a = "prod.aaa.u" + generate_uuid4().substr(0, 8);
    const std::string uid_b = "prod.bbb.u" + generate_uuid4().substr(0, 8);
    const fs::path path_a = vault_dir_ / "a.key";
    const fs::path path_b = vault_dir_ / "b.key";

    RoleVault::create(path_a, uid_a, kPassword, "rv:ct_a");
    RoleVault::create(path_b, uid_b, kPassword, "rv:ct_b");

    // Cross-uid open should fail (different KDF salt).
    EXPECT_THROW(RoleVault::open(path_a, uid_b, kPassword, "rv:ct_x1"), std::runtime_error);
    EXPECT_THROW(RoleVault::open(path_b, uid_a, kPassword, "rv:ct_x2"), std::runtime_error);
}

// ============================================================================
// Identity tests
// ============================================================================

TEST_F(RoleVaultTest, RoleUid_Roundtrip)
{
    RoleVault created = RoleVault::create(vault_path_, role_uid_, kPassword, "rv:uidrt_c");
    EXPECT_EQ(created.role_uid(), role_uid_);

    RoleVault opened = RoleVault::open(vault_path_, role_uid_, kPassword, "rv:uidrt_o");
    EXPECT_EQ(opened.role_uid(), role_uid_);
}

TEST_F(RoleVaultTest, Create_OverExistingVault_Throws_AtomicNoOverwrite)
{
    // HEP-CORE-0035 §4.6.1: vault write is atomic O_CREAT|O_EXCL.
    // A second create against an existing path MUST throw — silent
    // overwrite would invalidate any hub-side allowlist entry pinned
    // to the old pubkey, stranding the role.  Mutation-sweep against
    // the prior (pre-2026-06-01) contract.
    RoleVault v1 = RoleVault::create(vault_path_, role_uid_, kPassword, "rv:overwrite_1");
    const std::string sentinel_pk{v1.public_key()};

    // Pin the atomic-layer message — distinct from the operator-
    // friendly config-layer fs::exists() refusal that fires earlier
    // in --keygen.  Both must continue to refuse + cite the contract.
    try
    {
        (void)RoleVault::create(vault_path_, role_uid_, kPassword, "rv:overwrite_2");
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
    // The refused create must also leave no identity behind: a partial
    // create that banked a key would make the NEXT create fail with
    // "already present" instead of the real reason.
    EXPECT_FALSE(vsec::secure().keys().has("rv:overwrite_2"))
        << "a refused create left its minted identity in the key store";

    RoleVault still = RoleVault::open(vault_path_, role_uid_, kPassword, "rv:overwrite_reopen");
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

    EXPECT_THROW(RoleVault::create(vault_path_, role_uid_, kPassword, "rv:symlink"),
                 std::runtime_error)
        << "Create against a symlink at vault_path must refuse atomically";
    EXPECT_FALSE(vsec::secure().keys().has("rv:symlink"))
        << "a refused create left its minted identity in the key store";

    EXPECT_TRUE(fs::is_symlink(vault_path_));
    std::ifstream check(target);
    std::string content((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>{});
    EXPECT_EQ(content, "would receive redirected role secret")
        << "symlink target must remain untouched by refused create";
}

TEST_F(RoleVaultTest, Create_VaultFileIsMode0600_AndParentDirIs0700)
{
    const ::mode_t prev_umask = ::umask(0);
    RoleVault v = RoleVault::create(vault_path_, role_uid_, kPassword, "rv:mode");
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

TEST_F(RoleVaultTest, MoveConstructor_TransfersMetadata_KeyIsUnaffected)
{
    // CONTRACT CHANGED, deliberately.  This test used to assert that
    // moving a RoleVault "carried the secret half across", because the
    // vault owned the secret.  Under HEP-CORE-0035 §4.6.6 it does not
    // own one — `open`/`create` deposit the identity in the key store
    // and the vault keeps only public metadata.  Asserting the old
    // property would now be asserting nothing.
    //
    // What replaced it is a stronger statement worth pinning: the
    // identity's lifetime is the KEY STORE's, not the vault object's.
    // A moved-from — or destroyed — vault leaves the key exactly where
    // it was.
    const auto fp_before = [&]
    {
        RoleVault v1 = RoleVault::create(vault_path_, role_uid_, kPassword, "rv:move");
        const std::string pk{v1.public_key()};
        const std::string uid{v1.role_uid()};
        const auto fp = seckey_fingerprint("rv:move");

        RoleVault v2(std::move(v1));
        EXPECT_EQ(v2.public_key(), pk) << "move lost the public key";
        EXPECT_EQ(v2.role_uid(), uid) << "move lost the role uid";
        EXPECT_EQ(seckey_fingerprint("rv:move"), fp)
            << "moving the vault object disturbed the deposited identity";
        return fp;
        // both v1 and v2 destruct here
    }();

    EXPECT_TRUE(vsec::secure().keys().has("rv:move"))
        << "destroying the vault removed the identity — the key's lifetime must be the key "
           "store's, not the vault object's";
    EXPECT_EQ(seckey_fingerprint("rv:move"), fp_before)
        << "the identity changed after the vault that deposited it was destroyed";
}

TEST_F(RoleVaultTest, DifferentUids_DifferentKeys)
{
    const std::string uid_a = "cons.x.u" + generate_uuid4().substr(0, 8);
    const std::string uid_b = "cons.y.u" + generate_uuid4().substr(0, 8);
    const fs::path path_a = vault_dir_ / "a.key";
    const fs::path path_b = vault_dir_ / "b.key";

    RoleVault va = RoleVault::create(path_a, uid_a, kPassword, "rv:diff_a");
    RoleVault vb = RoleVault::create(path_b, uid_b, kPassword, "rv:diff_b");

    EXPECT_NE(va.public_key(), vb.public_key())
        << "Two vaults with different UIDs produced the same public key";
}
