/**
 * @file test_plh_hub_keygen.cpp
 * @brief plh_hub --keygen CLI tests.
 *
 * What --keygen does (HEP-CORE-0033 §6.5):
 *   - Reads hub.json, requires `hub.auth.keyfile` to be set.
 *   - Prompts for a vault password (or reads PYLABHUB_HUB_PASSWORD).
 *   - Writes <vault_path> with an encrypted CURVE keypair.
 *   - Prints the public key + vault path on stdout.
 *
 * Tests pin: vault file creation, public-key emission on stdout, error
 * when keyfile is unset.
 */

#include "plh_hub_fixture.h"

#include <cstdlib>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#endif

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::ExpectNoVaultArtifactsUnder;
using pylabhub::tests::helper::ExpectVaultDirSecured;
using pylabhub::tests::helper::ExpectVaultFileSecured;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

// Helper — set / unset PYLABHUB_HUB_PASSWORD around a test scope so
// the binary doesn't block on the interactive password prompt.
struct ScopedHubPassword
{
    explicit ScopedHubPassword(const std::string &pw)
    {
        ::setenv("PYLABHUB_HUB_PASSWORD", pw.c_str(), /*overwrite=*/1);
    }
    ~ScopedHubPassword() { ::unsetenv("PYLABHUB_HUB_PASSWORD"); }
};

// ── Success paths ────────────────────────────────────────────────────────────

TEST_F(PlhHubCliTest, GeneratesVaultFileAndEmitsPubkey)
{
    const auto dir = tmp("keygen_ok");
    const auto cfg_path = dir / "hub.json";

    // Hub vault filename embeds the hub UID (HEP-CORE-0033 §6.5,
    // revised 2026-05-31 — symmetric with role-side convention so
    // multiple hubs sharing a vault directory do not collide).  The
    // L4 fixture's minimal config uses hub.uid = "hub.l4test.uid00000001",
    // so the expected vault path is `vault/<that uid>.vault`.
    nlohmann::json overrides;
    overrides["hub"]["auth"]["keyfile"] = "vault/hub.l4test.uid00000001.vault";
    write_minimal_config(cfg_path, dir, overrides);
    const fs::path vault_actual = dir / "vault" / "hub.l4test.uid00000001.vault";

    ScopedHubPassword pw("test-password");
    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--keygen"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    expect_no_unexpected_errors(p);

    // (a) Vault file written with secure file mode (0600 on POSIX)
    //     and non-zero content.  Exit code alone is not reliable —
    //     a regression where the binary returns 0 but the chmod
    //     step is removed would pass the rc check; the mode check
    //     here catches it.  Equally a regression where the file is
    //     touched-empty (size 0) is caught by ExpectVaultFileSecured.
    ExpectVaultFileSecured(vault_actual);

    // (b) Vault parent dir MUST be 0700 (HEP-CORE-0035 §4.6.1).
    //     Enforced at write time by HubVault::create calling
    //     set_keyfile_mode(parent, VaultDir).  A regression that
    //     drops the explicit chmod and falls back to umask-derived
    //     mode (typically 0755) is caught here.
    ExpectVaultDirSecured(vault_actual.parent_path());

    // (c) stdout contains hub_uid + public_key (the operator-facing
    //     summary the hub binary prints after a successful keygen).
    //     This pins that the binary's stdout matches the documented
    //     operator-facing surface; rc=0 alone would not catch a
    //     regression that silenced the summary.
    EXPECT_NE(p.get_stdout().find("hub_uid"), std::string::npos) << "stdout missing hub_uid:\n"
                                                                 << p.get_stdout();
    EXPECT_NE(p.get_stdout().find("public_key"), std::string::npos)
        << "stdout missing public_key:\n"
        << p.get_stdout();
}

// ── HEP-CORE-0035 §4.6.4 publish-time observability ─────────────────────────

#if !defined(_WIN32) && !defined(_WIN64)
/// `HubVault::publish_public_key` (POSIX) unlinks any pre-existing
/// `hub.pubkey` before atomically O_CREAT|O_EXCL'ing the new one.
/// Round-2 B2 added a stderr `note:` whenever the unlink actually
/// removed something — operator-facing audit-trail evidence so a
/// silently-replaced symlink / regular file leaves a record.
/// This test pins the emission: a pre-planted hub.pubkey + --keygen
/// must surface the note.  A regression that silently drops the
/// emission (e.g., gated behind a verbose flag) would be invisible
/// otherwise.
TEST_F(PlhHubCliTest, KeygenEmitsNote_WhenPreExistingPubkeyRemoved)
{
    const auto dir = tmp("keygen_pre_pubkey");
    const auto cfg_path = dir / "hub.json";

    nlohmann::json overrides;
    overrides["hub"]["auth"]["keyfile"] = "vault/hub.l4test.uid00000002.vault";
    write_minimal_config(cfg_path, dir, overrides);

    // Pre-plant a sentinel hub.pubkey BEFORE --keygen runs.  The
    // binary's publish_public_key path will encounter an existing
    // path and unlink it, triggering the stderr note.
    const fs::path pubkey_path = dir / "hub.pubkey";
    {
        std::ofstream sentinel(pubkey_path);
        sentinel << "PRE-EXISTING SENTINEL — observability test marker";
    }
    ASSERT_TRUE(fs::exists(pubkey_path));

    ScopedHubPassword pw("note-test-pw");
    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--keygen"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();

    // The note text from hub_vault.cpp publish_public_key.  Pin
    // both the literal "pre-existing hub.pubkey" + the
    // "was removed before publish" phrase so a partial-rewording
    // refactor is caught.
    EXPECT_NE(p.get_stderr().find("pre-existing hub.pubkey"), std::string::npos)
        << "stderr should contain the B2 observability note; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("publish attempt follows"), std::string::npos)
        << "stderr should explain what happened next; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("HEP-CORE-0035 §4.6.4"), std::string::npos)
        << "note should cite the spec section; got:\n"
        << p.get_stderr();

    // Sentinel content was overwritten — verify by reading and
    // confirming the marker is gone (the new pubkey is a Z85 hex
    // string, not our sentinel).
    std::ifstream check(pubkey_path);
    std::string content((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>{});
    EXPECT_EQ(content.find("PRE-EXISTING SENTINEL"), std::string::npos)
        << "sentinel content survived publish — overwrite path broken";
}
#endif

// ── No-silent-overwrite contract (HEP-CORE-0033 §7.1) ───────────────────────

/// --keygen refuses to overwrite an existing vault file.  This is the
/// load-bearing check that the operator's existing CURVE keypair AND
/// admin token are not silently destroyed by a re-run of --keygen.
/// The test pre-creates a sentinel file at the resolved vault path
/// with KNOWN content, then runs --keygen, then verifies:
///   - rc != 0 (binary refused),
///   - stderr cites HEP-CORE-0033 and the resolved path,
///   - the sentinel file's CONTENT IS UNCHANGED (the load-bearing
///     contract — rc!=0 alone could pass even if the binary wrote
///     to a tmpfile then aborted; pinning the content catches a
///     regression that allowed any write).
TEST_F(PlhHubCliTest, KeygenRefusesToOverwriteExistingVault)
{
    const auto dir = tmp("keygen_no_overwrite");
    const auto cfg_path = dir / "hub.json";

    nlohmann::json overrides;
    overrides["hub"]["auth"]["keyfile"] = "vault/hub.l4test.uid00000001.vault";
    write_minimal_config(cfg_path, dir, overrides);
    const fs::path vault_actual = dir / "vault" / "hub.l4test.uid00000001.vault";

    // Pre-create the vault parent dir + a sentinel "vault" file with
    // known content + known mode.  The binary's --keygen must NOT
    // touch this file.
    fs::create_directories(vault_actual.parent_path());
    const std::string sentinel = "PRE-EXISTING VAULT — must not be overwritten by --keygen.\n"
                                 "If you see this content surviving a --keygen run that exited "
                                 "non-zero, the no-overwrite contract is intact.\n";
    {
        std::ofstream out(vault_actual, std::ios::binary);
        out << sentinel;
    }
    ASSERT_EQ(fs::file_size(vault_actual), sentinel.size());

    ScopedHubPassword pw("test-password");
    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--keygen"});
    const int rc = p.wait_for_exit();

    // (a) Binary refused.
    EXPECT_NE(rc, 0) << "expected non-zero exit; stdout:\n"
                     << p.get_stdout() << "\nstderr:\n"
                     << p.get_stderr();

    // (b) Diagnostic identifies the violated rule + path + HEP cite.
    EXPECT_NE(p.get_stderr().find("already exists"), std::string::npos)
        << "stderr should say the vault already exists; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find(vault_actual.string()), std::string::npos)
        << "stderr should name the offending path; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("HEP-CORE-0033"), std::string::npos)
        << "stderr should cite HEP-CORE-0033; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("rm"), std::string::npos)
        << "stderr should tell operator how to remove the file; got:\n"
        << p.get_stderr();
    // Hub-side specificity: the diagnostic names the two SECRETS the
    // overwrite would destroy — admin token + CURVE keypair.  Catches a
    // regression where the message gets genericized and stops telling
    // the operator *why* re-keygen is destructive on the hub side.
    EXPECT_NE(p.get_stderr().find("admin token"), std::string::npos)
        << "hub stderr should mention 'admin token' (hub-specific impact); "
           "got:\n"
        << p.get_stderr();

    // (c) THE LOAD-BEARING CHECK: file content IS UNCHANGED.  Reading
    //     the file back and comparing byte-for-byte against the
    //     sentinel — any mutation by the binary (truncation, partial
    //     write, full overwrite) breaks this assertion.
    std::string actual_content;
    {
        std::ifstream in(vault_actual, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        actual_content = ss.str();
    }
    EXPECT_EQ(actual_content, sentinel)
        << "no-overwrite contract VIOLATED — vault file content was "
           "modified by --keygen despite the refusal.  Expected:\n"
        << sentinel << "\nGot:\n"
        << actual_content;
    EXPECT_EQ(fs::file_size(vault_actual), sentinel.size())
        << "no-overwrite contract VIOLATED — vault file size changed";
}

// ── Runtime ACL discipline (HEP-CORE-0035 §4.6.2) ───────────────────────────

#if !defined(_WIN32) && !defined(_WIN64)
/// Runmode startup verifies vault file mode == 0600 before reading
/// the secret.  Loosened mode → refusal with HEP-CORE-0035 cite +
/// chmod hint.
TEST_F(PlhHubCliTest, RunmodeFailsWhenVaultFileModeIsLoose)
{
    const auto dir = tmp("rm_loose_mode");
    const auto cfg_path = dir / "hub.json";

    nlohmann::json overrides;
    overrides["hub"]["auth"]["keyfile"] = "vault/hub.l4loose.uid00000001.vault";
    write_minimal_config(cfg_path, dir, overrides);
    const fs::path vault_path = dir / "vault" / "hub.l4loose.uid00000001.vault";

    {
        ScopedHubPassword pw("loose-pw");
        WorkerProcess kg(plh_hub_binary(), "--config", {cfg_path.string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << "setup keygen failed: " << kg.get_stderr();
    }

    // Loosen mode to world-readable.  Spawn the binary in runmode
    // (no flag) — it must refuse before reading the secret.
    ::chmod(vault_path.c_str(), 0644);

    {
        ScopedHubPassword pw("loose-pw");
        WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string()});
        const int rc = p.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S);
        EXPECT_NE(rc, 0) << "runmode against vault at 0644 must refuse; stderr:\n"
                         << p.get_stderr();
        EXPECT_NE(p.get_stderr().find("HEP-CORE-0035"), std::string::npos)
            << "stderr should cite HEP-CORE-0035 §4.6.2; got:\n"
            << p.get_stderr();
        // Pin "vault file" (NOT "vault directory") so a regression
        // that swaps the file diagnostic for the dir diagnostic is
        // caught.  Pin the exact target mode (0600) so a regression
        // emitting "chmod 0644" or "chmod 0700" (wrong target for
        // file) is caught.
        EXPECT_NE(p.get_stderr().find("vault file"), std::string::npos)
            << "diagnostic should identify the FILE check fired; got:\n"
            << p.get_stderr();
        EXPECT_NE(p.get_stderr().find("chmod 0600"), std::string::npos)
            << "stderr should tell operator to chmod 0600 (exact); got:\n"
            << p.get_stderr();
    }
}

/// Symmetric: parent dir at 0755 → runmode refuses.
TEST_F(PlhHubCliTest, RunmodeFailsWhenVaultParentDirModeIsLoose)
{
    const auto dir = tmp("rm_loose_dir");
    const auto cfg_path = dir / "hub.json";

    nlohmann::json overrides;
    overrides["hub"]["auth"]["keyfile"] = "vault/hub.l4loose.uid00000002.vault";
    write_minimal_config(cfg_path, dir, overrides);
    const fs::path vault_path = dir / "vault" / "hub.l4loose.uid00000002.vault";

    {
        ScopedHubPassword pw("loose-pw");
        WorkerProcess kg(plh_hub_binary(), "--config", {cfg_path.string(), "--keygen"});
        ASSERT_EQ(kg.wait_for_exit(), 0) << "setup keygen failed: " << kg.get_stderr();
    }

    ::chmod(vault_path.parent_path().c_str(), 0755);

    {
        ScopedHubPassword pw("loose-pw");
        WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string()});
        const int rc = p.wait_for_exit(PYLABHUB_TEST_CRYPTO_TIMEOUT_S);
        EXPECT_NE(rc, 0) << "runmode against vault dir at 0755 must refuse; stderr:\n"
                         << p.get_stderr();
        EXPECT_NE(p.get_stderr().find("HEP-CORE-0035"), std::string::npos)
            << "stderr should cite HEP-CORE-0035 §4.6.2; got:\n"
            << p.get_stderr();
        // Pin "vault directory" (NOT "vault file") + exact target
        // mode 0700 so the test catches a regression that swaps file
        // vs dir diagnostics or emits the wrong target mode.
        EXPECT_NE(p.get_stderr().find("vault directory"), std::string::npos)
            << "diagnostic should identify the DIR check fired; got:\n"
            << p.get_stderr();
        EXPECT_NE(p.get_stderr().find("chmod 0700"), std::string::npos)
            << "stderr should tell operator to chmod 0700 (exact); got:\n"
            << p.get_stderr();
    }
}
#endif

// ── Error paths ──────────────────────────────────────────────────────────────

/// Empty `hub.auth.keyfile` is rejected at config-parse time
/// (HEP-CORE-0033 §7.1) — pylabhub is a vault and requires a path.
/// Tests that the parse-time rejection fires before any --keygen
/// work begins.  Replaces the prior contract that allowed empty
/// keyfile and rejected it at --keygen specifically.
TEST_F(PlhHubCliTest, FailsWhenKeyfileEmpty)
{
    const auto dir = tmp("keygen_empty_keyfile");
    const auto cfg_path = dir / "hub.json";

    nlohmann::json overrides;
    overrides["hub"]["auth"]["keyfile"] = ""; // explicitly empty
    write_minimal_config(cfg_path, dir, overrides);

    ScopedHubPassword pw("test-password");
    WorkerProcess p(plh_hub_binary(), "--config", {cfg_path.string(), "--keygen"});
    const int rc = p.wait_for_exit();
    EXPECT_NE(rc, 0) << "expected non-zero exit; stdout:\n"
                     << p.get_stdout() << "\nstderr:\n"
                     << p.get_stderr();

    // (a) Diagnostic identifies the failing field AND the contract violated.
    EXPECT_NE(p.get_stderr().find("auth.keyfile"), std::string::npos)
        << "stderr should mention auth.keyfile; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("non-empty"), std::string::npos)
        << "stderr should describe the empty-string violation; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("HEP-CORE-0024"), std::string::npos)
        << "stderr should cite the source-of-truth HEP; got:\n"
        << p.get_stderr();

    // (b) No vault file (or any *.vault artifact) was created — the
    //     config-load guard fires BEFORE any --keygen filesystem work.
    //     A regression where the parse-time error is set but write
    //     still proceeds would leave a vault on disk; this catches it.
    ExpectNoVaultArtifactsUnder(dir);
}

} // namespace
