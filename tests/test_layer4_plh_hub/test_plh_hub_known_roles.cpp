/**
 * @file test_plh_hub_known_roles.cpp
 * @brief plh_hub --add-known-role / --revoke-known-role / --list-known-roles
 *        CLI behavior tests (PeerAdmission Phase C close-out, commit 4/5).
 *
 * These tests pin the file-state side effects of the operator-facing
 * known-role CLI ops — what file appears on disk, what content is in
 * it, what doesn't change when nothing should change.  Pre-fix code had
 * two foot-guns flagged by the fresh-eye review:
 *
 *   - H-K3: `--revoke-known-role <uid>` wrote `known_roles.json`
 *     unconditionally.  An operator typo on a fresh hub_dir would
 *     materialize an empty deny-all file, silently flipping intent
 *     from "no allowlist" to "deny everyone."  Even on an existing
 *     file, a typo'd revoke advanced the file's mtime without any
 *     content change, corrupting forensic audit timelines.
 *
 *   - H-K4: `--add-known-role <name> <uid> <role> <pubkey_z85>`
 *     accepted arbitrary strings in <role>.  A typo persisted into
 *     the allowlist where the broker's case-sensitive role check
 *     silently never matched — leading to denied handshakes the
 *     operator could not diagnose.  (H-K4 parse-time validation is
 *     covered by L2 test_hub_cli.cpp; this file pins binary-level
 *     exit + stderr behavior.)
 *
 * Pattern: each test spawns the staged plh_hub binary as a subprocess
 * with a fresh tmp hub_dir, runs the CLI op, and inspects exit code,
 * stdout, stderr, and the resulting filesystem state.
 */

#include "plh_hub_fixture.h"

#include <sys/stat.h>
#include <chrono>
#include <thread>

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

constexpr const char *kValidPubkey1 =
    "0123456789012345678901234567890123456789";
constexpr const char *kValidPubkey2 =
    "ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ";

/// Check whether <hub_dir>/vault/known_roles.json exists.
bool known_roles_exists(const fs::path &hub_dir)
{
    std::error_code ec;
    return fs::exists(hub_dir / "vault" / "known_roles.json", ec);
}

} // namespace

// ── H-K3 — revoke side effects ─────────────────────────────────────────────

/// **H-K3 fix proof — fresh hub_dir.**  Revoking a non-existent uid on
/// a hub_dir with no known_roles.json must NOT create the file.  The
/// pre-fix code unconditionally saved (creating an empty deny-all
/// file from a typo) — flipping operator intent silently.
TEST_F(PlhHubCliTest, RevokeUnknownUid_OnFreshHubDir_DoesNotCreateFile)
{
    const auto hub_dir = tmp("revoke_fresh");

    WorkerProcess p(plh_hub_binary(), hub_dir.string(),
                    {"--revoke-known-role", "uid.never.existed"});
    EXPECT_EQ(p.wait_for_exit(), 0)
        << "stderr:\n" << p.get_stderr();
    EXPECT_FALSE(known_roles_exists(hub_dir))
        << "known_roles.json must NOT exist after revoking on a fresh "
           "hub_dir — that would silently flip operator intent from "
           "'no allowlist' to 'deny-all'.  stderr:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stdout().find("not present"), std::string::npos)
        << "stdout must explicitly signal the no-op; got:\n"
        << p.get_stdout();
    EXPECT_NE(p.get_stdout().find("file untouched"), std::string::npos)
        << "stdout must signal that the file was not modified so the "
           "operator knows their typo had no effect; got:\n"
        << p.get_stdout();
}

/// **H-K3 fix proof — existing file untouched.**  Revoking a uid that
/// is NOT in an existing known_roles.json must leave the file
/// content-AND-mtime unchanged.  Pre-fix code rewrote the file (same
/// content, new mtime), polluting forensic audit timelines.
TEST_F(PlhHubCliTest, RevokeUnknownUid_OnExistingFile_LeavesFileUntouched)
{
    const auto hub_dir = tmp("revoke_existing_unknown");

    // Seed: add alice via the binary itself (so file is well-formed).
    {
        WorkerProcess seed(plh_hub_binary(), hub_dir.string(),
                           {"--add-known-role", "alice", "uid.alice",
                            "producer", kValidPubkey1});
        ASSERT_EQ(seed.wait_for_exit(), 0)
            << "seed failed:\n" << seed.get_stderr();
    }
    const auto known_roles = hub_dir / "vault" / "known_roles.json";
    ASSERT_TRUE(fs::exists(known_roles));

    // Capture pre-revoke state.
    struct ::stat st_before{};
    ASSERT_EQ(::stat(known_roles.c_str(), &st_before), 0);
    const std::string content_before =
        [&]{ std::ifstream f(known_roles); std::ostringstream s; s << f.rdbuf(); return s.str(); }();

    // Sleep beyond mtime resolution so an unconditional write would
    // produce a detectable mtime delta on this filesystem.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Revoke a uid that ISN'T present.
    WorkerProcess p(plh_hub_binary(), hub_dir.string(),
                    {"--revoke-known-role", "uid.does.not.exist"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    EXPECT_NE(p.get_stdout().find("not present"), std::string::npos);
    EXPECT_NE(p.get_stdout().find("file untouched"), std::string::npos);

    // Pin: mtime unchanged AND content unchanged.
    struct ::stat st_after{};
    ASSERT_EQ(::stat(known_roles.c_str(), &st_after), 0);
    EXPECT_EQ(st_before.st_mtime, st_after.st_mtime)
        << "Revoking a non-existent uid must not advance mtime "
           "(forensic timeline corruption risk).";
    const std::string content_after =
        [&]{ std::ifstream f(known_roles); std::ostringstream s; s << f.rdbuf(); return s.str(); }();
    EXPECT_EQ(content_before, content_after);
}

/// Control: revoking a KNOWN uid does remove it and rewrite the file.
/// Pin both the content change AND that the remaining entry survives.
TEST_F(PlhHubCliTest, RevokeKnownUid_RemovesEntryAndRewritesFile)
{
    const auto hub_dir = tmp("revoke_known");

    // Seed two entries so we can verify the second survives.
    {
        WorkerProcess seedA(plh_hub_binary(), hub_dir.string(),
                            {"--add-known-role", "alice", "uid.alice",
                             "producer", kValidPubkey1});
        ASSERT_EQ(seedA.wait_for_exit(), 0);
        WorkerProcess seedB(plh_hub_binary(), hub_dir.string(),
                            {"--add-known-role", "bob", "uid.bob",
                             "consumer", kValidPubkey2});
        ASSERT_EQ(seedB.wait_for_exit(), 0);
    }
    const auto known_roles = hub_dir / "vault" / "known_roles.json";
    ASSERT_TRUE(fs::exists(known_roles));

    // Revoke alice.
    WorkerProcess p(plh_hub_binary(), hub_dir.string(),
                    {"--revoke-known-role", "uid.alice"});
    EXPECT_EQ(p.wait_for_exit(), 0) << "stderr:\n" << p.get_stderr();
    EXPECT_NE(p.get_stdout().find("revoked"), std::string::npos);
    EXPECT_NE(p.get_stdout().find("uid.alice"), std::string::npos);

    // File still exists; alice gone; bob present.
    ASSERT_TRUE(fs::exists(known_roles));
    const auto j = read_json(known_roles);
    ASSERT_TRUE(j.contains("roles") && j["roles"].is_array());
    ASSERT_EQ(j["roles"].size(), 1u);
    EXPECT_EQ(j["roles"][0].value("uid", ""), "uid.bob");
    EXPECT_EQ(j["roles"][0].value("pubkey_z85", ""), kValidPubkey2);
}

// ── H-K4 — role enum at binary level ───────────────────────────────────────

/// **H-K4 fix proof — binary tier.**  L2 test_hub_cli pins the parser
/// in isolation; this test pins the operator-visible behavior of the
/// installed binary on a typo'd role.  The binary must exit 1 and
/// emit a diagnostic naming the bad value AND the allowed set.  No
/// file is created.
TEST_F(PlhHubCliTest, AddKnownRole_TypoRole_ExitsOneAndCreatesNoFile)
{
    const auto hub_dir = tmp("add_typo");

    WorkerProcess p(plh_hub_binary(), hub_dir.string(),
                    {"--add-known-role", "alice", "uid.alice",
                     "prodcer",   // typo
                     kValidPubkey1});
    EXPECT_EQ(p.wait_for_exit(), 1)
        << "Typo'd role must produce non-zero exit; stderr:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("'prodcer'"), std::string::npos)
        << "diagnostic must quote the bad value; got:\n"
        << p.get_stderr();
    EXPECT_NE(p.get_stderr().find("producer"), std::string::npos)
        << "diagnostic must enumerate the allowed roles; got:\n"
        << p.get_stderr();
    EXPECT_FALSE(known_roles_exists(hub_dir))
        << "Failed add must not leave a partially-written file behind.";
}

/// Control: each valid role string is accepted AND produces a
/// well-formed known_roles.json containing the entry.
TEST_F(PlhHubCliTest, AddKnownRole_EachValidRole_AcceptedAndPersisted)
{
    struct Case { const char *role; const char *pubkey; };
    const std::vector<Case> cases{
        {"producer",  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},
        {"consumer",  "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"},
        {"processor", "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"},
        {"any",       "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"},
    };

    for (const auto &c : cases)
    {
        const auto hub_dir =
            tmp(std::string("add_valid_") + c.role);
        WorkerProcess p(plh_hub_binary(), hub_dir.string(),
                        {"--add-known-role", "n",
                         std::string("uid.") + c.role,
                         c.role, c.pubkey});
        EXPECT_EQ(p.wait_for_exit(), 0)
            << "role='" << c.role << "'; stderr:\n" << p.get_stderr();
        ASSERT_TRUE(known_roles_exists(hub_dir));
        const auto j = read_json(hub_dir / "vault" / "known_roles.json");
        ASSERT_TRUE(j.contains("roles"));
        ASSERT_EQ(j["roles"].size(), 1u);
        EXPECT_EQ(j["roles"][0].value("role", ""), c.role);
        EXPECT_EQ(j["roles"][0].value("pubkey_z85", ""), c.pubkey);
    }
}
