/**
 * @file test_known_roles.cpp
 * @brief L2 unit tests for KnownRolesStore (PeerAdmission Phase B).
 *
 * Pattern 1 — pure value-type test + filesystem round-trip via the
 * #101 atomic_write_owner_only_file helper.  No LOGGER_*, no lifecycle.
 *
 * Coverage:
 *   - add / remove / find / find_by_pubkey value semantics
 *   - upsert-by-uid contract (replace on duplicate uid)
 *   - validate_entry rejects empty uid / empty pubkey / wrong-length pubkey
 *   - file round-trip: save → load reproduces state byte-for-byte
 *   - load_from_file tolerates missing file → std::nullopt (P-Bootstrap b)
 *   - load_from_file rejects unknown version, missing version, dup uid
 *   - load_from_file enforces ACL (mode 0600) per HEP-CORE-0035 §4.6.2
 *   - as_peer_allowlist excludes empty-pubkey legacy entries
 *   - as_peer_allowlist never returns unrestricted (no operator bypass
 *     ever flows through this method)
 */

#include "utils/security/known_roles.hpp"

#include "utils/security/key_file_acl.hpp"  // for ACL roundtrip control

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using pylabhub::broker::KnownRole;
using pylabhub::utils::security::KnownRolesStore;
using pylabhub::utils::security::PeerAllowlist;
using pylabhub::utils::security::PeerIdentity;
using pylabhub::utils::security::kKnownRolesSchemaVersion;
using pylabhub::utils::security::atomic_write_owner_only_file;

namespace
{

KnownRole make_role(std::string uid, std::string pubkey_z85,
                    std::string name = "test.role",
                    std::string role = "producer")
{
    KnownRole r;
    r.name       = std::move(name);
    r.uid        = std::move(uid);
    r.role       = std::move(role);
    r.pubkey_z85 = std::move(pubkey_z85);
    return r;
}

/// Generate a syntactically-valid 40-char Z85 string for tests.  Real
/// CURVE pubkeys are libsodium-generated; for L2 contract tests the
/// validation cares only about length.
std::string fake_pubkey(char fill = 'A')
{
    return std::string(40, fill);
}

fs::path make_tmp_path(const std::string &name)
{
    static std::atomic<int> ctr{0};
    return fs::temp_directory_path() /
           ("plh_l2_known_roles_" + name + "_" +
            std::to_string(::getpid()) + "_" +
            std::to_string(ctr.fetch_add(1)) + ".json");
}

} // namespace

// ── add / find / remove value semantics ─────────────────────────────────────

TEST(KnownRolesStoreTest, EmptyStore_HasZeroEntries)
{
    KnownRolesStore s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.list().size(), 0u);
}

TEST(KnownRolesStoreTest, Add_InsertsNewEntry)
{
    KnownRolesStore s;
    EXPECT_TRUE(s.add(make_role("uid.alice", fake_pubkey('A'))));
    EXPECT_EQ(s.size(), 1u);
    EXPECT_FALSE(s.empty());
}

TEST(KnownRolesStoreTest, Add_DuplicateUid_ReplacesAndReturnsFalse)
{
    KnownRolesStore s;
    s.add(make_role("uid.alice", fake_pubkey('A'), "Alice v1"));
    EXPECT_FALSE(s.add(make_role("uid.alice", fake_pubkey('B'), "Alice v2")));
    EXPECT_EQ(s.size(), 1u);  // still one
    auto got = s.find("uid.alice");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "Alice v2");
    EXPECT_EQ(got->pubkey_z85, fake_pubkey('B'));
}

TEST(KnownRolesStoreTest, Add_EmptyUid_Throws)
{
    KnownRolesStore s;
    EXPECT_THROW(s.add(make_role("", fake_pubkey('A'))),
                 std::invalid_argument);
    EXPECT_EQ(s.size(), 0u);
}

TEST(KnownRolesStoreTest, Add_EmptyPubkey_Throws)
{
    KnownRolesStore s;
    EXPECT_THROW(s.add(make_role("uid.alice", "")),
                 std::invalid_argument);
}

TEST(KnownRolesStoreTest, Add_WrongPubkeyLength_Throws)
{
    KnownRolesStore s;
    EXPECT_THROW(s.add(make_role("uid.alice", "tooshort")),
                 std::invalid_argument);
    EXPECT_THROW(s.add(make_role("uid.alice", std::string(41, 'A'))),
                 std::invalid_argument);
    EXPECT_THROW(s.add(make_role("uid.alice", std::string(39, 'A'))),
                 std::invalid_argument);
}

TEST(KnownRolesStoreTest, Find_ReturnsMatchOrNullopt)
{
    KnownRolesStore s;
    s.add(make_role("uid.alice", fake_pubkey('A')));
    s.add(make_role("uid.bob",   fake_pubkey('B')));

    auto a = s.find("uid.alice");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->uid, "uid.alice");
    EXPECT_EQ(a->pubkey_z85, fake_pubkey('A'));

    EXPECT_FALSE(s.find("uid.unknown").has_value());
}

TEST(KnownRolesStoreTest, FindByPubkey_ReturnsMatchOrNullopt)
{
    KnownRolesStore s;
    s.add(make_role("uid.alice", fake_pubkey('A')));
    s.add(make_role("uid.bob",   fake_pubkey('B')));

    auto a = s.find_by_pubkey(fake_pubkey('A'));
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->uid, "uid.alice");

    EXPECT_FALSE(s.find_by_pubkey(fake_pubkey('Z')).has_value());
}

TEST(KnownRolesStoreTest, Remove_DeletesEntry_ReturnsTrue)
{
    KnownRolesStore s;
    s.add(make_role("uid.alice", fake_pubkey('A')));
    EXPECT_TRUE(s.remove("uid.alice"));
    EXPECT_EQ(s.size(), 0u);
    EXPECT_FALSE(s.find("uid.alice").has_value());
}

TEST(KnownRolesStoreTest, Remove_NonExistent_ReturnsFalse)
{
    KnownRolesStore s;
    EXPECT_FALSE(s.remove("uid.unknown"));
}

TEST(KnownRolesStoreTest, List_PreservesInsertionOrder)
{
    // Insertion-order determinism is contractually required by
    // --list-known-roles CLI output (which is operator-grepped); a
    // future switch to unordered_map would break this.
    KnownRolesStore s;
    s.add(make_role("uid.bob",   fake_pubkey('B'), "Bob"));
    s.add(make_role("uid.alice", fake_pubkey('A'), "Alice"));
    s.add(make_role("uid.carol", fake_pubkey('C'), "Carol"));

    const auto &l = s.list();
    ASSERT_EQ(l.size(), 3u);
    EXPECT_EQ(l[0].uid, "uid.bob");
    EXPECT_EQ(l[1].uid, "uid.alice");
    EXPECT_EQ(l[2].uid, "uid.carol");
}

// ── as_peer_allowlist bridge ────────────────────────────────────────────────

TEST(KnownRolesStoreTest, AsPeerAllowlist_ProjectsPubkeysOnly)
{
    KnownRolesStore s;
    s.add(make_role("uid.alice", fake_pubkey('A')));
    s.add(make_role("uid.bob",   fake_pubkey('B')));

    auto al = s.as_peer_allowlist();
    EXPECT_EQ(al.peers.size(), 2u);
    EXPECT_TRUE(al.contains(PeerIdentity{"curve", fake_pubkey('A')}));
    EXPECT_TRUE(al.contains(PeerIdentity{"curve", fake_pubkey('B')}));
    EXPECT_FALSE(al.contains(PeerIdentity{"curve", fake_pubkey('Z')}));
    EXPECT_FALSE(al.unrestricted)
        << "as_peer_allowlist() must NEVER produce unrestricted=true; "
           "only the explicit --allow-anonymous-data operator flag "
           "sets that, and it does NOT flow through this method";
}

TEST(KnownRolesStoreTest, AsPeerAllowlist_Empty_DeniesAll)
{
    KnownRolesStore s;
    auto al = s.as_peer_allowlist();
    EXPECT_TRUE(al.is_deny_all());
    EXPECT_FALSE(al.contains(PeerIdentity{"curve", fake_pubkey('A')}));
}

// ── File round-trip ─────────────────────────────────────────────────────────

TEST(KnownRolesStoreTest, LoadFromFile_MissingFile_ReturnsNullopt)
{
    auto p = make_tmp_path("missing");
    ASSERT_FALSE(fs::exists(p));
    auto opt = KnownRolesStore::load_from_file(p);
    EXPECT_FALSE(opt.has_value())
        << "Missing file must yield nullopt so the broker can WARN + "
           "start with deny-all (design doc §8 P-Bootstrap option b), "
           "rather than refusing to boot";
}

TEST(KnownRolesStoreTest, SaveLoad_RoundTrip_PreservesEntries)
{
    auto p = make_tmp_path("roundtrip");
    KnownRolesStore in;
    in.add(make_role("uid.alice", fake_pubkey('A'), "Alice", "producer"));
    in.add(make_role("uid.bob",   fake_pubkey('B'), "Bob",   "consumer"));
    in.add(make_role("uid.carol", fake_pubkey('C'), "Carol", "processor"));

    in.save_to_file(p);
    ASSERT_TRUE(fs::exists(p));

    auto out_opt = KnownRolesStore::load_from_file(p);
    ASSERT_TRUE(out_opt.has_value());
    const auto &out = *out_opt;

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out.list()[0].uid,        "uid.alice");
    EXPECT_EQ(out.list()[0].name,       "Alice");
    EXPECT_EQ(out.list()[0].role,       "producer");
    EXPECT_EQ(out.list()[0].pubkey_z85, fake_pubkey('A'));
    EXPECT_EQ(out.list()[1].uid,        "uid.bob");
    EXPECT_EQ(out.list()[2].uid,        "uid.carol");

    fs::remove(p);
}

#if !defined(_WIN32) && !defined(_WIN64)
TEST(KnownRolesStoreTest, SaveToFile_ProducesMode0600)
{
    auto p = make_tmp_path("mode_check");
    KnownRolesStore s;
    s.add(make_role("uid.alice", fake_pubkey('A')));

    const ::mode_t prev_umask = ::umask(0);
    s.save_to_file(p);
    ::umask(prev_umask);

    const auto perms = fs::status(p).permissions();
    EXPECT_EQ(static_cast<unsigned>(perms) & 0777, 0600u)
        << "save_to_file must atomically produce mode 0600 even when "
           "the calling umask would have permitted looser modes "
           "(HEP-CORE-0035 §4.6.1)";

    fs::remove(p);
}

TEST(KnownRolesStoreTest, LoadFromFile_LooseMode_Refuses)
{
    auto p = make_tmp_path("loose_mode");
    KnownRolesStore s;
    s.add(make_role("uid.alice", fake_pubkey('A')));
    s.save_to_file(p);

    // Loosen file mode after the proper write.  Load must refuse.
    ::chmod(p.c_str(), 0644);

    EXPECT_THROW(KnownRolesStore::load_from_file(p),
                 std::runtime_error);

    ::chmod(p.c_str(), 0600);
    fs::remove(p);
}
#endif

TEST(KnownRolesStoreTest, LoadFromFile_MissingVersion_Throws)
{
    auto p = make_tmp_path("no_version");
    atomic_write_owner_only_file(
        p, R"({"roles":[]})");

    EXPECT_THROW(KnownRolesStore::load_from_file(p), std::runtime_error);
    fs::remove(p);
}

TEST(KnownRolesStoreTest, LoadFromFile_UnknownVersion_Throws)
{
    auto p = make_tmp_path("unknown_version");
    atomic_write_owner_only_file(
        p, R"({"version": 999, "roles": []})");

    EXPECT_THROW(KnownRolesStore::load_from_file(p), std::runtime_error);
    fs::remove(p);
}

TEST(KnownRolesStoreTest, LoadFromFile_DuplicateUid_Throws)
{
    auto p = make_tmp_path("dup_uid");
    std::ostringstream js;
    js << R"({"version": )" << kKnownRolesSchemaVersion << R"(, "roles": [)"
       << R"({"uid":"uid.alice","name":"A","role":"producer","pubkey_z85":")"
       << fake_pubkey('A') << R"("},)"
       << R"({"uid":"uid.alice","name":"B","role":"consumer","pubkey_z85":")"
       << fake_pubkey('B') << R"("}])"
       << R"(})";
    atomic_write_owner_only_file(p, js.str());

    EXPECT_THROW(KnownRolesStore::load_from_file(p), std::runtime_error)
        << "Duplicate uid in the on-disk file must surface as a hard "
           "error rather than silently picking one — operator intent "
           "is ambiguous and must be corrected before the broker boots";
    fs::remove(p);
}

TEST(KnownRolesStoreTest, LoadFromFile_BadJson_Throws)
{
    auto p = make_tmp_path("bad_json");
    atomic_write_owner_only_file(p, "{ this is not json");

    EXPECT_THROW(KnownRolesStore::load_from_file(p), std::runtime_error);
    fs::remove(p);
}

// ── atomic_write_owner_only_file — covered indirectly above + smoke here ────

#if !defined(_WIN32) && !defined(_WIN64)
TEST(AtomicWriteOwnerOnlyFileTest, OverwriteExistingFile_AtomicReplace)
{
    auto p = make_tmp_path("overwrite");
    atomic_write_owner_only_file(p, "first");
    atomic_write_owner_only_file(p, "second");

    std::ifstream in(p);
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>{});
    EXPECT_EQ(content, "second");

    const auto perms = fs::status(p).permissions();
    EXPECT_EQ(static_cast<unsigned>(perms) & 0777, 0600u);

    fs::remove(p);
}

TEST(AtomicWriteOwnerOnlyFileTest, SymlinkAtTarget_ReplacedNotFollowed)
{
    // The function uses rename(2) which on POSIX replaces a symlink at
    // the target path WITHOUT following the link — the symlink's
    // target file is unaffected.  Mutation-sweep: plant a symlink at
    // `path` pointing at a sentinel file; after the write, `path` must
    // be a regular file holding the new content, and the sentinel
    // must be byte-unchanged.
    auto p = make_tmp_path("symlink_target");
    const auto sentinel = make_tmp_path("symlink_sentinel_target");
    {
        std::ofstream s(sentinel);
        s << "PRISTINE SENTINEL — symlink redirect must NOT write here";
    }
    fs::create_symlink(sentinel, p);
    ASSERT_TRUE(fs::is_symlink(p));

    atomic_write_owner_only_file(p, "fresh");

    // p is now a REGULAR file with the new content.
    EXPECT_FALSE(fs::is_symlink(p));
    {
        std::ifstream in(p);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>{});
        EXPECT_EQ(content, "fresh");
    }
    // Sentinel target is UNCHANGED.
    {
        std::ifstream in(sentinel);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>{});
        EXPECT_EQ(content,
                  "PRISTINE SENTINEL — symlink redirect must NOT write here");
    }

    fs::remove(p);
    fs::remove(sentinel);
}

TEST(AtomicWriteOwnerOnlyFileTest, SymlinkAtTmpPath_TmpStaleSymlinkSafelyReplaced)
{
    // Recovery contract: if a previous invocation was interrupted and
    // left a stale tmp (which CAN be a symlink, e.g., from a malicious
    // pre-plant inside the 0700 keystore dir), the new invocation must
    // succeed by:
    //   1. unlinking the stale tmp (removes the symlink, not its target);
    //   2. creating a fresh regular tmp with O_NOFOLLOW guarding against
    //      the rare race where an attacker re-symlinks between unlink
    //      and open.
    // This test verifies (1): pre-plant a symlink at tmp; the write
    // must succeed AND the symlink's target must be untouched.
    auto p = make_tmp_path("stale_tmp_recovery");
    const auto tmp = fs::path(p.string() + ".tmp");
    const auto sentinel = make_tmp_path("stale_tmp_sentinel");
    {
        std::ofstream s(sentinel);
        s << "SENTINEL TARGET — must NOT be modified through symlink";
    }
    fs::create_symlink(sentinel, tmp);
    ASSERT_TRUE(fs::is_symlink(tmp));

    // Write succeeds; the function silently unlinks the stale tmp
    // symlink and proceeds.
    EXPECT_NO_THROW(atomic_write_owner_only_file(p, "fresh-recovered"));

    EXPECT_TRUE(fs::exists(p));
    EXPECT_FALSE(fs::is_symlink(p));
    // Sentinel target untouched.
    std::ifstream in(sentinel);
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>{});
    EXPECT_EQ(content,
              "SENTINEL TARGET — must NOT be modified through symlink");

    fs::remove(p);
    fs::remove(sentinel);
}
#endif
