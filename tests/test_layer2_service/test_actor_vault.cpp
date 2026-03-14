/**
 * @file test_actor_vault.cpp
 * @brief Unit tests for ActorVault: create, open, encryption verification.
 *
 * Mirrors the HubVault test pattern. ActorVault is simpler (no admin_token,
 * no publish_public_key) — stores only public_key + secret_key + actor_uid.
 *
 * Pure API test — no lifecycle, no workers. Each test gets an isolated temp
 * directory. Argon2id KDF takes ~0.5s per call; timeout set to 120s.
 */
#include "utils/actor_vault.hpp"
#include "utils/uuid_utils.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "plh_platform.hpp"

namespace fs = std::filesystem;
using pylabhub::utils::ActorVault;
using pylabhub::utils::generate_uuid4;

namespace
{

constexpr const char *kPassword      = "test-actor-password-123";
constexpr const char *kWrongPassword = "wrong-password-456";

/// Validate a Z85 key: exactly 40 printable ASCII chars (no space).
bool is_valid_z85_key(const std::string &s)
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

class ActorVaultTest : public ::testing::Test
{
protected:
    fs::path    vault_dir_;
    fs::path    vault_path_;
    std::string actor_uid_;

    void SetUp() override
    {
        actor_uid_ = "PROD-TEST-" + generate_uuid4().substr(0, 8);
        vault_dir_ =
            fs::temp_directory_path() / ("plh_test_actor_vault_" + generate_uuid4().substr(0, 8));
        fs::create_directories(vault_dir_);
        vault_path_ = vault_dir_ / "actor.key";
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

TEST_F(ActorVaultTest, Create_WritesFile)
{
    ActorVault::create(vault_path_, actor_uid_, kPassword);
    ASSERT_TRUE(fs::exists(vault_path_)) << "Vault file not created";
    EXPECT_GT(fs::file_size(vault_path_), 40u) << "Vault file suspiciously small";
}

TEST_F(ActorVaultTest, Create_RestrictedPerms)
{
#if !defined(PYLABHUB_PLATFORM_WIN64)
    // Windows has no POSIX file modes; skip permission check.
    ActorVault::create(vault_path_, actor_uid_, kPassword);

    const fs::perms p = fs::status(vault_path_).permissions();
    EXPECT_EQ(p & fs::perms::group_read, fs::perms::none)  << "group_read should be off";
    EXPECT_EQ(p & fs::perms::group_write, fs::perms::none) << "group_write should be off";
    EXPECT_EQ(p & fs::perms::others_read, fs::perms::none) << "others_read should be off";
    EXPECT_EQ(p & fs::perms::others_write, fs::perms::none) << "others_write should be off";
#else
    GTEST_SKIP() << "File permission test not applicable on Windows";
#endif
}

TEST_F(ActorVaultTest, Create_ValidZ85Keypair)
{
    ActorVault v = ActorVault::create(vault_path_, actor_uid_, kPassword);
    EXPECT_TRUE(is_valid_z85_key(v.public_key()))
        << "public_key is not a valid 40-char Z85 key: '" << v.public_key() << "'";
    EXPECT_TRUE(is_valid_z85_key(v.secret_key()))
        << "secret_key is not a valid 40-char Z85 key";
}

TEST_F(ActorVaultTest, Create_EmptyPassword)
{
    // Dev-mode: empty password is allowed.
    EXPECT_NO_THROW(ActorVault::create(vault_path_, actor_uid_, ""));
    EXPECT_TRUE(fs::exists(vault_path_));
}

// ============================================================================
// Open tests
// ============================================================================

TEST_F(ActorVaultTest, Open_CorrectPassword)
{
    ActorVault created = ActorVault::create(vault_path_, actor_uid_, kPassword);
    ActorVault opened  = ActorVault::open(vault_path_, actor_uid_, kPassword);

    EXPECT_EQ(created.public_key(), opened.public_key());
    EXPECT_EQ(created.secret_key(), opened.secret_key());
    EXPECT_EQ(created.actor_uid(), opened.actor_uid());
}

TEST_F(ActorVaultTest, Open_WrongPassword_Throws)
{
    ActorVault::create(vault_path_, actor_uid_, kPassword);
    EXPECT_THROW(ActorVault::open(vault_path_, actor_uid_, kWrongPassword), std::runtime_error);
}

TEST_F(ActorVaultTest, Open_CorruptedFile_Throws)
{
    ActorVault::create(vault_path_, actor_uid_, kPassword);

    // Flip bytes in the ciphertext (after the 24-byte nonce).
    {
        std::fstream f(vault_path_, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekp(30, std::ios::beg);
        const char garbage[] = {'\xDE', '\xAD', '\xBE', '\xEF'};
        f.write(garbage, sizeof(garbage));
    }
#if !defined(PYLABHUB_PLATFORM_WIN64)
    fs::permissions(vault_path_, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
#endif

    EXPECT_THROW(ActorVault::open(vault_path_, actor_uid_, kPassword), std::runtime_error);
}

TEST_F(ActorVaultTest, Open_MissingFile_Throws)
{
    EXPECT_THROW(ActorVault::open(vault_path_, actor_uid_, kPassword), std::runtime_error);
}

// ============================================================================
// Encryption verification
// ============================================================================

TEST_F(ActorVaultTest, Encrypt_SecretsNotInPlaintext)
{
    ActorVault v = ActorVault::create(vault_path_, actor_uid_, kPassword);

    std::ifstream ifs(vault_path_, std::ios::binary);
    const std::string raw_bytes((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());

    EXPECT_EQ(raw_bytes.find(v.public_key()), std::string::npos)
        << "Public key appears in plaintext in vault file";
    EXPECT_EQ(raw_bytes.find(v.secret_key()), std::string::npos)
        << "Secret key appears in plaintext in vault file";
}

TEST_F(ActorVaultTest, Encrypt_DifferentUid_DifferentCiphertext)
{
    const std::string uid_a = "PROD-AAA-" + generate_uuid4().substr(0, 8);
    const std::string uid_b = "PROD-BBB-" + generate_uuid4().substr(0, 8);
    const fs::path path_a = vault_dir_ / "a.key";
    const fs::path path_b = vault_dir_ / "b.key";

    ActorVault::create(path_a, uid_a, kPassword);
    ActorVault::create(path_b, uid_b, kPassword);

    // Cross-uid open should fail (different KDF salt).
    EXPECT_THROW(ActorVault::open(path_a, uid_b, kPassword), std::runtime_error);
    EXPECT_THROW(ActorVault::open(path_b, uid_a, kPassword), std::runtime_error);
}

// ============================================================================
// Identity tests
// ============================================================================

TEST_F(ActorVaultTest, ActorUid_Roundtrip)
{
    ActorVault created = ActorVault::create(vault_path_, actor_uid_, kPassword);
    EXPECT_EQ(created.actor_uid(), actor_uid_);

    ActorVault opened = ActorVault::open(vault_path_, actor_uid_, kPassword);
    EXPECT_EQ(opened.actor_uid(), actor_uid_);
}

TEST_F(ActorVaultTest, Create_OverExistingVault_Overwrites)
{
    ActorVault v1 = ActorVault::create(vault_path_, actor_uid_, kPassword);
    const std::string pk1 = v1.public_key();

    // Create again on same path — should overwrite with fresh keys.
    ActorVault v2 = ActorVault::create(vault_path_, actor_uid_, kPassword);
    const std::string pk2 = v2.public_key();

    EXPECT_NE(pk1, pk2) << "Second create should generate fresh keys";
}

TEST_F(ActorVaultTest, MoveConstructor_TransfersOwnership)
{
    ActorVault v1 = ActorVault::create(vault_path_, actor_uid_, kPassword);
    const std::string pk = v1.public_key();
    const std::string sk = v1.secret_key();
    const std::string uid = v1.actor_uid();

    ActorVault v2(std::move(v1));
    EXPECT_EQ(v2.public_key(), pk);
    EXPECT_EQ(v2.secret_key(), sk);
    EXPECT_EQ(v2.actor_uid(), uid);
}

TEST_F(ActorVaultTest, DifferentUids_DifferentKeys)
{
    const std::string uid_a = "CONS-X-" + generate_uuid4().substr(0, 8);
    const std::string uid_b = "CONS-Y-" + generate_uuid4().substr(0, 8);
    const fs::path path_a = vault_dir_ / "a.key";
    const fs::path path_b = vault_dir_ / "b.key";

    ActorVault va = ActorVault::create(path_a, uid_a, kPassword);
    ActorVault vb = ActorVault::create(path_b, uid_b, kPassword);

    EXPECT_NE(va.public_key(), vb.public_key())
        << "Two vaults with different UIDs produced the same public key";
}
