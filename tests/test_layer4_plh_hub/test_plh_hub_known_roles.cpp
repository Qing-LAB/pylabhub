/**
 * @file test_plh_hub_known_roles.cpp
 * @brief plh_hub --add-known-role / --revoke-known-role / --list-known-roles
 *        / --migrate-known-roles CLI behavior tests (HEP-CORE-0035 §4.8).
 *
 * The known_roles allowlist lives INSIDE the encrypted hub vault, not a
 * plaintext `known_roles.json` sidecar.  Every mutating/reading command
 * unlocks the vault with the master password (`PYLABHUB_HUB_PASSWORD`) and
 * re-encrypts on change.  These tests pin the operator-visible behavior of
 * the installed binary against the vault:
 *
 *   - add/revoke/list round-trip through the encrypted vault (assertions
 *     read the vault back via `--list-known-roles`, never a plaintext file).
 *   - H-K3 (preserved): a no-op revoke of an absent uid does NOT re-encrypt
 *     the vault (its mtime is unchanged), avoiding audit-timeline noise.
 *   - H-K4 (preserved): a typo'd <role> is rejected at parse time, exit 1.
 *   - §4.8.7 hard cutover: `--migrate-known-roles` imports a legacy
 *     plaintext file into the vault and deletes it; and the run/validate
 *     path REFUSES to start while a plaintext `known_roles.json` exists.
 *
 * Pattern: each test spawns the staged plh_hub binary against a fresh
 * keygen'd hub (hub.json + encrypted vault) and inspects exit code, stdout,
 * stderr, and vault state.
 */

#include "plh_hub_fixture.h"

#include "utils/role_identity_policy.hpp" // pylabhub::broker::KnownRole
#include "utils/security/known_roles.hpp" // KnownRolesStore (legacy-file author)

#include <sys/stat.h>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

constexpr const char *kValidPubkey1 = "0123456789012345678901234567890123456789";
constexpr const char *kValidPubkey2 = "ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ";
constexpr const char *kHubPass = "l4-known-roles-pw";

/// Path of the encrypted vault for a hub built by `write_minimal_config`
/// (which sets `auth.keyfile = "vault/placeholder.vault"`).
fs::path vault_path_of(const fs::path &hub_dir)
{
    return hub_dir / "vault" / "placeholder.vault";
}

/// Provision a keygen'd hub (hub.json + encrypted vault) and return its
/// config path.  Caller MUST hold a live `ScopedHubPassword`.
fs::path prepare_keygened_hub(const fs::path &hub_dir)
{
    const fs::path cfg = hub_dir / "hub.json";
    write_minimal_config(cfg, hub_dir);
    keygen_minimal_hub(cfg); // mints the encrypted vault (Argon2id)
    return cfg;
}

/// Run a plh_hub known-role op against @p cfg (via `--config`).
WorkerProcess run_op(const fs::path &cfg, const std::vector<std::string> &op)
{
    std::vector<std::string> rest{cfg.string()};
    rest.insert(rest.end(), op.begin(), op.end());
    return WorkerProcess(plh_hub_binary(), "--config", rest);
}

} // namespace

// ── H-K3 — revoke side effects (vault edition) ─────────────────────────────

/// Revoking an absent uid on an empty (bootstrap) vault exits 0, signals the
/// no-op, and leaves the vault untouched.
TEST_F(PlhHubCliTest, RevokeUnknownUid_OnEmptyVault_ExitsZeroNotPresent)
{
    ScopedHubPassword pw(kHubPass);
    const auto hub_dir = tmp("revoke_empty");
    const auto cfg = prepare_keygened_hub(hub_dir);

    auto p = run_op(cfg, {"--revoke-known-role", "uid.never.existed"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    EXPECT_NE(p.get_stdout().find("not present"), std::string::npos)
        << "stdout must signal the no-op; got:\n"
        << p.get_stdout();
    EXPECT_NE(p.get_stdout().find("vault untouched"), std::string::npos)
        << "stdout must signal the vault was not rewritten; got:\n"
        << p.get_stdout();
}

/// H-K3 preserved: revoking a uid that isn't present must NOT re-encrypt the
/// vault — its mtime is unchanged (no audit-timeline pollution).
TEST_F(PlhHubCliTest, RevokeUnknownUid_AfterAdd_LeavesVaultMtimeUnchanged)
{
    ScopedHubPassword pw(kHubPass);
    const auto hub_dir = tmp("revoke_unknown_mtime");
    const auto cfg = prepare_keygened_hub(hub_dir);
    const auto vault = vault_path_of(hub_dir);

    {
        auto a = run_op(cfg, {"--add-known-role", "alice", "uid.alice", "producer", kValidPubkey1});
        ASSERT_EQ(a.wait_for_exit(), 0) << "seed failed:\n" << a.get_stderr();
    }
    ASSERT_TRUE(fs::exists(vault));

    struct ::stat before{};
    ASSERT_EQ(::stat(vault.c_str(), &before), 0);

    // Sleep beyond mtime resolution so an unconditional re-encrypt would
    // produce a detectable mtime delta.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto p = run_op(cfg, {"--revoke-known-role", "uid.does.not.exist"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    EXPECT_NE(p.get_stdout().find("not present"), std::string::npos);

    struct ::stat after{};
    ASSERT_EQ(::stat(vault.c_str(), &after), 0);
    EXPECT_EQ(before.st_mtime, after.st_mtime)
        << "Revoking a non-existent uid must not re-encrypt the vault "
           "(forensic timeline corruption risk).";

    // alice still admitted.
    auto l = run_op(cfg, {"--list-known-roles"});
    ASSERT_EQ(l.wait_for_exit(), 0) << l.get_stderr();
    EXPECT_NE(l.get_stdout().find("uid.alice"), std::string::npos) << l.get_stdout();
}

/// Control: revoking a KNOWN uid removes it; the other entry survives.
TEST_F(PlhHubCliTest, RevokeKnownUid_RemovesEntry_KeepsOthers)
{
    ScopedHubPassword pw(kHubPass);
    const auto hub_dir = tmp("revoke_known");
    const auto cfg = prepare_keygened_hub(hub_dir);

    {
        auto a = run_op(cfg, {"--add-known-role", "alice", "uid.alice", "producer", kValidPubkey1});
        ASSERT_EQ(a.wait_for_exit(), 0) << a.get_stderr();
        auto b = run_op(cfg, {"--add-known-role", "bob", "uid.bob", "consumer", kValidPubkey2});
        ASSERT_EQ(b.wait_for_exit(), 0) << b.get_stderr();
    }

    auto p = run_op(cfg, {"--revoke-known-role", "uid.alice"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    EXPECT_NE(p.get_stdout().find("revoked"), std::string::npos);
    EXPECT_NE(p.get_stdout().find("uid.alice"), std::string::npos);

    auto l = run_op(cfg, {"--list-known-roles"});
    ASSERT_EQ(l.wait_for_exit(), 0) << l.get_stderr();
    const std::string out = l.get_stdout();
    EXPECT_EQ(out.find("uid.alice"), std::string::npos) << "alice must be gone:\n" << out;
    EXPECT_NE(out.find("uid.bob"), std::string::npos) << "bob must survive:\n" << out;
    EXPECT_NE(out.find(kValidPubkey2), std::string::npos) << out;
}

// ── H-K4 — role enum at binary level ───────────────────────────────────────

/// H-K4 preserved: a typo'd <role> is rejected at parse time (before any
/// vault access) — exit 1 with a diagnostic naming the bad value + allowed
/// set.  No keygen needed: the parser rejects before dispatch.
TEST_F(PlhHubCliTest, AddKnownRole_TypoRole_ExitsOne)
{
    const auto hub_dir = tmp("add_typo");

    WorkerProcess p(plh_hub_binary(), hub_dir.string(),
                    {"--add-known-role", "alice", "uid.alice",
                     "prodcer", // typo
                     kValidPubkey1});
    EXPECT_EQ(p.wait_for_exit(), 1) << "Typo'd role must produce non-zero exit; stderr:\n"
                                    << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("'prodcer'"), std::string::npos)
        << "diagnostic must quote the bad value; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("producer"), std::string::npos)
        << "diagnostic must enumerate the allowed roles; got:\n"
        << p.get_stderr();
}

/// Control: each valid role string is accepted and appears in the vault
/// allowlist (read back via --list-known-roles).
TEST_F(PlhHubCliTest, AddKnownRole_EachValidRole_AcceptedAndListed)
{
    struct Case
    {
        const char *role;
        const char *pubkey;
    };
    const std::vector<Case> cases{
        {"producer", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},
        {"consumer", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"},
        {"processor", "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"},
        {"any", "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"},
    };

    for (const auto &c : cases)
    {
        ScopedHubPassword pw(kHubPass);
        const auto hub_dir = tmp(std::string("add_valid_") + c.role);
        const auto cfg = prepare_keygened_hub(hub_dir);

        auto p =
            run_op(cfg, {"--add-known-role", "n", std::string("uid.") + c.role, c.role, c.pubkey});
        EXPECT_EQ(p.wait_for_exit(), 0) << "role='" << c.role << "'; stderr:\n" << p.get_stderr();

        auto l = run_op(cfg, {"--list-known-roles"});
        ASSERT_EQ(l.wait_for_exit(), 0) << l.get_stderr();
        const std::string out = l.get_stdout();
        EXPECT_NE(out.find(std::string("role='") + c.role + "'"), std::string::npos)
            << "role='" << c.role << "' not listed:\n"
            << out;
        EXPECT_NE(out.find(c.pubkey), std::string::npos) << out;
    }
}

// ── §4.8.7 hard cutover ────────────────────────────────────────────────────

/// `--migrate-known-roles` imports a legacy plaintext allowlist into the
/// vault and deletes the file.
TEST_F(PlhHubCliTest, MigrateKnownRoles_ImportsPlaintextIntoVaultAndDeletesFile)
{
    ScopedHubPassword pw(kHubPass);
    const auto hub_dir = tmp("migrate");
    const auto cfg = prepare_keygened_hub(hub_dir);

    // Author a legacy plaintext known_roles.json via the store's file codec.
    const fs::path legacy = hub_dir / "vault" / "known_roles.json";
    {
        pylabhub::utils::security::KnownRolesStore store;
        pylabhub::broker::KnownRole e;
        e.name = "legacy";
        e.uid = "uid.legacy";
        e.role = "producer";
        e.pubkey_z85 = kValidPubkey1;
        store.add(std::move(e));
        store.save_to_file(legacy);
    }
    ASSERT_TRUE(fs::exists(legacy));

    auto m = run_op(cfg, {"--migrate-known-roles"});
    EXPECT_EQ(m.wait_for_exit(), 0) << "stderr:\n" << m.get_stderr();
    EXPECT_FALSE(fs::exists(legacy)) << "migrate must delete the plaintext file after importing it";

    auto l = run_op(cfg, {"--list-known-roles"});
    ASSERT_EQ(l.wait_for_exit(), 0) << l.get_stderr();
    EXPECT_NE(l.get_stdout().find("uid.legacy"), std::string::npos)
        << "migrated role must be in the vault:\n"
        << l.get_stdout();
}

/// The run/validate path REFUSES to start while a plaintext known_roles.json
/// exists (hard cutover), naming the file and the migrate command.
TEST_F(PlhHubCliTest, Validate_RefusesWhenPlaintextKnownRolesPresent)
{
    ScopedHubPassword pw(kHubPass);
    const auto hub_dir = tmp("cutover");
    const auto cfg = prepare_keygened_hub(hub_dir);

    // Drop a stray plaintext allowlist.
    const fs::path legacy = hub_dir / "vault" / "known_roles.json";
    {
        std::ofstream out(legacy);
        out << R"({"version":1,"roles":[]})";
    }
    ASSERT_TRUE(fs::exists(legacy));

    WorkerProcess v(plh_hub_binary(), "--config", {cfg.string(), "--validate"});
    EXPECT_NE(v.wait_for_exit(), 0)
        << "hub must refuse to start with a plaintext allowlist present";
    EXPECT_NE(v.get_stderr().find("known_roles.json"), std::string::npos)
        << "diagnostic must name the offending file; got:\n"
        << v.get_stderr();
    EXPECT_NE(v.get_stderr().find("migrate-known-roles"), std::string::npos)
        << "diagnostic must point at the migration command; got:\n"
        << v.get_stderr();
}
