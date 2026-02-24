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
#include "utils/hub_identity.hpp"
#include "utils/hub_vault.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using pylabhub::utils::HubVault;
using pylabhub::utils::generate_uuid4;

namespace
{

// Shared test constants
constexpr const char *kPassword = "test-password-123";
constexpr const char *kWrongPassword = "wrong-password-456";

// Validate a Z85 key: exactly 40 printable ASCII chars.
bool is_valid_z85_key(const std::string &s)
{
    if (s.size() != 40)
        return false;
    for (char c : s)
        if (c < 0x21 || c > 0x7E) // printable ASCII, no space
            return false;
    return true;
}

// Validate a 64-char lowercase hex token.
bool is_valid_hex_token(const std::string &s)
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

class HubVaultTest : public ::testing::Test
{
protected:
    fs::path hub_dir_;
    std::string hub_uid_;

    void SetUp() override
    {
        hub_uid_ = generate_uuid4();
        hub_dir_ = fs::temp_directory_path() /
                   ("pylabhub_vault_test_" + hub_uid_.substr(0, 8));
        fs::create_directories(hub_dir_);
    }

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
    HubVault::create(hub_dir_, hub_uid_, kPassword);

    const fs::path vault_path = hub_dir_ / "hub.vault";
    ASSERT_TRUE(fs::exists(vault_path)) << "hub.vault not created";
    EXPECT_GT(fs::file_size(vault_path), 40u) << "hub.vault suspiciously small";
}

TEST_F(HubVaultTest, CreateVaultFileHasRestrictedPermissions)
{
    HubVault::create(hub_dir_, hub_uid_, kPassword);

    const fs::path vault_path = hub_dir_ / "hub.vault";
    const fs::perms p = fs::status(vault_path).permissions();
    // Must be owner read/write only (0600) — no group/other bits.
    EXPECT_EQ(p & fs::perms::group_read,    fs::perms::none) << "group_read should be off";
    EXPECT_EQ(p & fs::perms::group_write,   fs::perms::none) << "group_write should be off";
    EXPECT_EQ(p & fs::perms::others_read,   fs::perms::none) << "others_read should be off";
    EXPECT_EQ(p & fs::perms::others_write,  fs::perms::none) << "others_write should be off";
}

TEST_F(HubVaultTest, CreateReturnsValidZ85Keypair)
{
    HubVault v = HubVault::create(hub_dir_, hub_uid_, kPassword);

    EXPECT_TRUE(is_valid_z85_key(v.broker_curve_public_key()))
        << "broker_curve_public_key is not a valid 40-char Z85 key: '"
        << v.broker_curve_public_key() << "'";
    EXPECT_TRUE(is_valid_z85_key(v.broker_curve_secret_key()))
        << "broker_curve_secret_key is not a valid 40-char Z85 key";
}

TEST_F(HubVaultTest, CreateReturnsValid64CharHexAdminToken)
{
    HubVault v = HubVault::create(hub_dir_, hub_uid_, kPassword);

    EXPECT_TRUE(is_valid_hex_token(v.admin_token()))
        << "admin_token is not a 64-char hex string: '" << v.admin_token() << "'";
}

TEST_F(HubVaultTest, EmptyPasswordCreatesVaultSuccessfully)
{
    // Dev-mode: empty password is allowed (weak but functional).
    EXPECT_NO_THROW(HubVault::create(hub_dir_, hub_uid_, ""));
    EXPECT_TRUE(fs::exists(hub_dir_ / "hub.vault"));
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

    HubVault va = HubVault::create(dir_a, uid_a, kPassword);
    HubVault vb = HubVault::create(dir_b, uid_b, kPassword);

    EXPECT_NE(va.broker_curve_public_key(), vb.broker_curve_public_key())
        << "Two vaults produced the same public key — RNG failure?";
    EXPECT_NE(va.admin_token(), vb.admin_token())
        << "Two vaults produced the same admin token — RNG failure?";
}

// ============================================================================
// Open tests
// ============================================================================

TEST_F(HubVaultTest, OpenWithCorrectPasswordReturnsMatchingSecrets)
{
    HubVault created = HubVault::create(hub_dir_, hub_uid_, kPassword);
    HubVault opened  = HubVault::open(hub_dir_, hub_uid_, kPassword);

    EXPECT_EQ(created.broker_curve_public_key(), opened.broker_curve_public_key());
    EXPECT_EQ(created.broker_curve_secret_key(), opened.broker_curve_secret_key());
    EXPECT_EQ(created.admin_token(),             opened.admin_token());
}

TEST_F(HubVaultTest, OpenWithWrongPasswordThrows)
{
    HubVault::create(hub_dir_, hub_uid_, kPassword);

    EXPECT_THROW(HubVault::open(hub_dir_, hub_uid_, kWrongPassword), std::runtime_error);
}

TEST_F(HubVaultTest, OpenCorruptedVaultThrows)
{
    HubVault::create(hub_dir_, hub_uid_, kPassword);

    // Flip bytes in the middle of the ciphertext (after the 24-byte nonce).
    const fs::path vault_path = hub_dir_ / "hub.vault";
    {
        std::fstream f(vault_path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekp(30, std::ios::beg); // inside the MAC-protected ciphertext
        const char garbage[] = {'\xDE', '\xAD', '\xBE', '\xEF'};
        f.write(garbage, sizeof(garbage));
    }
    // Restore 0600 permissions (fstream may not preserve them)
    fs::permissions(vault_path,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);

    EXPECT_THROW(HubVault::open(hub_dir_, hub_uid_, kPassword), std::runtime_error);
}

TEST_F(HubVaultTest, OpenMissingVaultThrows)
{
    // No create() call — vault file does not exist.
    EXPECT_THROW(HubVault::open(hub_dir_, hub_uid_, kPassword), std::runtime_error);
}

// ============================================================================
// Encryption verification tests
// ============================================================================

TEST_F(HubVaultTest, VaultFileDoesNotContainPlaintextSecrets)
{
    // The vault must actually encrypt its payload — raw bytes in the file should
    // not contain the Z85 keys or the admin token as printable substrings.
    HubVault v = HubVault::create(hub_dir_, hub_uid_, kPassword);

    std::ifstream ifs(hub_dir_ / "hub.vault", std::ios::binary);
    const std::string raw_bytes((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());

    EXPECT_EQ(raw_bytes.find(v.broker_curve_public_key()), std::string::npos)
        << "Broker public key appears in plaintext in hub.vault — encryption is not working!";
    EXPECT_EQ(raw_bytes.find(v.broker_curve_secret_key()), std::string::npos)
        << "Broker secret key appears in plaintext in hub.vault — encryption is not working!";
    EXPECT_EQ(raw_bytes.find(v.admin_token()), std::string::npos)
        << "Admin token appears in plaintext in hub.vault — encryption is not working!";
}

TEST_F(HubVaultTest, EncryptDecryptRoundTrip)
{
    // Full roundtrip: the secrets written by create() must come back unchanged
    // after open(). This verifies the encrypt → file → decrypt pipeline
    // end-to-end with known values.
    HubVault created = HubVault::create(hub_dir_, hub_uid_, kPassword);

    const std::string expected_pubkey = created.broker_curve_public_key();
    const std::string expected_seckey = created.broker_curve_secret_key();
    const std::string expected_token  = created.admin_token();

    // Simulate a new process opening the vault (discard the in-memory object).
    HubVault reopened = HubVault::open(hub_dir_, hub_uid_, kPassword);

    EXPECT_EQ(reopened.broker_curve_public_key(), expected_pubkey)
        << "Public key changed after encrypt/decrypt roundtrip";
    EXPECT_EQ(reopened.broker_curve_secret_key(), expected_seckey)
        << "Secret key changed after encrypt/decrypt roundtrip";
    EXPECT_EQ(reopened.admin_token(), expected_token)
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

    HubVault::create(dir_a, uid_a, kPassword);
    HubVault::create(dir_b, uid_b, kPassword);

    // Opening vault A with uid_b (wrong salt) must fail.
    EXPECT_THROW(HubVault::open(dir_a, uid_b, kPassword), std::runtime_error)
        << "Cross-uid open should fail (wrong KDF salt)";

    // Opening vault B with uid_a must also fail.
    EXPECT_THROW(HubVault::open(dir_b, uid_a, kPassword), std::runtime_error)
        << "Cross-uid open should fail (wrong KDF salt)";
}

// ============================================================================
// publish_public_key tests
// ============================================================================

TEST_F(HubVaultTest, PublishPublicKeyWritesCorrectContent)
{
    HubVault v = HubVault::create(hub_dir_, hub_uid_, kPassword);
    v.publish_public_key(hub_dir_);

    const fs::path pubkey_path = hub_dir_ / "hub.pubkey";
    ASSERT_TRUE(fs::exists(pubkey_path)) << "hub.pubkey not written";

    std::ifstream ifs(pubkey_path);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content, v.broker_curve_public_key())
        << "hub.pubkey content does not match vault public key";
}

TEST_F(HubVaultTest, PublishPublicKeyHasWorldReadablePermissions)
{
    HubVault v = HubVault::create(hub_dir_, hub_uid_, kPassword);
    v.publish_public_key(hub_dir_);

    const fs::path pubkey_path = hub_dir_ / "hub.pubkey";
    const fs::perms p = fs::status(pubkey_path).permissions();
    // Must be 0644: owner rw, group r, others r.
    EXPECT_NE(p & fs::perms::owner_read,  fs::perms::none) << "owner_read should be set";
    EXPECT_NE(p & fs::perms::owner_write, fs::perms::none) << "owner_write should be set";
    EXPECT_NE(p & fs::perms::group_read,  fs::perms::none) << "group_read should be set";
    EXPECT_NE(p & fs::perms::others_read, fs::perms::none) << "others_read should be set";
    EXPECT_EQ(p & fs::perms::others_write, fs::perms::none) << "others_write should be off";
}
