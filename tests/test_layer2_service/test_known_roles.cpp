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

// ── H-K1: strict-type extraction (close-out commit 3) ──────────────────────
//
// nlohmann::json::value(k, default) returns `default` BOTH when k is
// missing AND when k has the wrong JSON type.  The pre-fix
// entry_from_json silently coerced typed mistakes — e.g.,
// `"pubkey_z85": null` and `"name": 1234` — to empty strings, breaking
// save→load identity AND hiding operator-edit mistakes from the
// validator.  These tests pin that the strict-typed extraction rejects
// wrong-type fields with a precise diagnostic.

TEST(KnownRolesStoreTest, LoadFromFile_PubkeyWrongType_Number_Throws)
{
    auto p = make_tmp_path("pubkey_wrongtype_number");
    std::ostringstream js;
    js << R"({"version": )" << kKnownRolesSchemaVersion << R"(, "roles": [)"
       << R"({"uid":"uid.alice","name":"A","role":"producer",)"
          R"("pubkey_z85": 42}])"
       << R"(})";
    atomic_write_owner_only_file(p, js.str());

    try {
        (void) KnownRolesStore::load_from_file(p);
        FAIL() << "expected throw on wrong-type pubkey_z85 (got number)";
    } catch (const std::runtime_error &e) {
        // Pin the diagnostic specifically — it must name the field
        // AND the observed type.  A generic "load failed" would not
        // help the operator find the bad line in known_roles.json.
        const std::string what(e.what());
        EXPECT_NE(what.find("pubkey_z85"), std::string::npos) << what;
        EXPECT_NE(what.find("string"), std::string::npos) << what;
    }
    fs::remove(p);
}

TEST(KnownRolesStoreTest, LoadFromFile_NameWrongType_Array_Throws)
{
    auto p = make_tmp_path("name_wrongtype_array");
    std::ostringstream js;
    js << R"({"version": )" << kKnownRolesSchemaVersion << R"(, "roles": [)"
       << R"({"uid":"uid.alice","name":["x","y"],"role":"producer",)"
          R"("pubkey_z85":")" << fake_pubkey('A') << R"("}])"
       << R"(})";
    atomic_write_owner_only_file(p, js.str());

    try {
        (void) KnownRolesStore::load_from_file(p);
        FAIL() << "expected throw on wrong-type name (got array)";
    } catch (const std::runtime_error &e) {
        const std::string what(e.what());
        EXPECT_NE(what.find("name"), std::string::npos) << what;
        EXPECT_NE(what.find("string"), std::string::npos) << what;
    }
    fs::remove(p);
}

TEST(KnownRolesStoreTest, LoadFromFile_UidWrongType_Number_Throws)
{
    auto p = make_tmp_path("uid_wrongtype_number");
    std::ostringstream js;
    js << R"({"version": )" << kKnownRolesSchemaVersion << R"(, "roles": [)"
       << R"({"uid": 1234,"name":"alice","role":"producer",)"
          R"("pubkey_z85":")" << fake_pubkey('A') << R"("}])"
       << R"(})";
    atomic_write_owner_only_file(p, js.str());

    try {
        (void) KnownRolesStore::load_from_file(p);
        FAIL() << "expected throw on wrong-type uid (got number)";
    } catch (const std::runtime_error &e) {
        const std::string what(e.what());
        EXPECT_NE(what.find("uid"), std::string::npos) << what;
        EXPECT_NE(what.find("string"), std::string::npos) << what;
    }
    fs::remove(p);
}

TEST(KnownRolesStoreTest, LoadFromFile_RoleWrongType_Bool_Throws)
{
    auto p = make_tmp_path("role_wrongtype_bool");
    std::ostringstream js;
    js << R"({"version": )" << kKnownRolesSchemaVersion << R"(, "roles": [)"
       << R"({"uid":"uid.alice","name":"alice","role": true,)"
          R"("pubkey_z85":")" << fake_pubkey('A') << R"("}])"
       << R"(})";
    atomic_write_owner_only_file(p, js.str());

    try {
        (void) KnownRolesStore::load_from_file(p);
        FAIL() << "expected throw on wrong-type role (got bool)";
    } catch (const std::runtime_error &e) {
        const std::string what(e.what());
        EXPECT_NE(what.find("role"), std::string::npos) << what;
        EXPECT_NE(what.find("string"), std::string::npos) << what;
    }
    fs::remove(p);
}

TEST(KnownRolesStoreTest, LoadFromFile_PubkeyNull_FailsValidation)
{
    // JSON null is treated as "missing" by require_string_or_empty
    // (the field collapses to ""), then validate_entry catches the
    // empty pubkey via std::invalid_argument.  This pins the cleanup
    // chain: null is not the same as wrong-type; it's a permitted
    // omission that the semantic validator still rejects.
    auto p = make_tmp_path("pubkey_null");
    std::ostringstream js;
    js << R"({"version": )" << kKnownRolesSchemaVersion << R"(, "roles": [)"
       << R"({"uid":"uid.alice","name":"alice","role":"producer",)"
          R"("pubkey_z85": null}])"
       << R"(})";
    atomic_write_owner_only_file(p, js.str());

    try {
        (void) KnownRolesStore::load_from_file(p);
        FAIL() << "expected throw — empty pubkey rejected at validator";
    } catch (const std::invalid_argument &e) {
        // validate_entry uses std::invalid_argument for empty pubkey.
        const std::string what(e.what());
        EXPECT_NE(what.find("pubkey"), std::string::npos) << what;
    } catch (const std::exception &e) {
        FAIL() << "expected std::invalid_argument from validate_entry "
                  "(empty pubkey path); got: " << e.what();
    }
    fs::remove(p);
}

TEST(KnownRolesStoreTest, LoadFromFile_MissingOptionalNameAndRole_LoadsWithEmpty)
{
    // Backward-compat: legacy files that omit name/role still load.
    // require_string_or_empty treats missing field as "" (not a type
    // error), and validate_entry only requires uid + pubkey.
    auto p = make_tmp_path("missing_optional");
    std::ostringstream js;
    js << R"({"version": )" << kKnownRolesSchemaVersion << R"(, "roles": [)"
       << R"({"uid":"uid.alice","pubkey_z85":")" << fake_pubkey('A') << R"("}])"
       << R"(})";
    atomic_write_owner_only_file(p, js.str());

    auto loaded = KnownRolesStore::load_from_file(p);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->size(), 1u);
    EXPECT_EQ(loaded->list()[0].uid, "uid.alice");
    EXPECT_EQ(loaded->list()[0].name, "");
    EXPECT_EQ(loaded->list()[0].role, "");
    EXPECT_EQ(loaded->list()[0].pubkey_z85, fake_pubkey('A'));
    fs::remove(p);
}

#if !defined(_WIN32) && !defined(_WIN64)
// ── H-K2: parent-directory ACL advisory surfacing ──────────────────────────
//
// verify_keyfile_acl(VaultFile) returns `ok=true` with a non-empty
// `diagnostic` when the file passes its own ACL check (0600, owned)
// but the parent directory has group/world bits set.  The pre-fix
// load_from_file silently swallowed that diagnostic.  Operators
// auditing the file would have no signal that their <hub_dir>/vault/
// is leaking the existence of known_roles.json.
//
// This test sets the parent dir to mode 0750, writes a valid 0600
// file, and pins that load_from_file (a) succeeds AND (b) prints the
// advisory to stderr containing the parent-directory diagnostic.
TEST(KnownRolesStoreTest, LoadFromFile_GroupReadableParentDir_SurfacesAdvisory)
{
    // Custom temp dir we can chmod (the system temp dir is typically
    // 1777, which would also trigger but isn't OUR mode to assert).
    static std::atomic<int> ctr{0};
    fs::path parent = fs::temp_directory_path() /
                      ("plh_l2_known_roles_advisory_" +
                       std::to_string(::getpid()) + "_" +
                       std::to_string(ctr.fetch_add(1)));
    fs::create_directories(parent);
    // Force parent to mode 0750 — group-readable, world-search-only.
    ::chmod(parent.c_str(), 0750);

    fs::path p = parent / "known_roles.json";
    KnownRolesStore s;
    s.add(make_role("uid.alice", fake_pubkey('A')));
    s.save_to_file(p);  // writes 0600

    testing::internal::CaptureStderr();
    auto loaded = KnownRolesStore::load_from_file(p);
    const std::string err = testing::internal::GetCapturedStderr();

    ASSERT_TRUE(loaded.has_value())
        << "Load must succeed — the advisory is non-fatal per "
           "HEP-CORE-0035 §4.6.2";
    ASSERT_EQ(loaded->size(), 1u);
    EXPECT_NE(err.find("KnownRolesStore"), std::string::npos)
        << "Advisory must be tagged so the operator can grep for it; "
           "stderr was:\n" << err;
    EXPECT_NE(err.find("parent directory"), std::string::npos)
        << "Advisory must mention the parent-dir leak signal; "
           "stderr was:\n" << err;

    // Restore parent mode so cleanup works on hosts with strict umask.
    ::chmod(parent.c_str(), 0700);
    fs::remove(p);
    fs::remove(parent);
}
#endif

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

// ─── HEP-CORE-0036 §I10 — one pubkey per role uid (sep. of duties) ──────────
//
// The invariant is enforced in `add()` and `load_from_file()`; the
// disposition at compile time is exposed via the free function
// `known_roles_enforces_unique_pubkey()`.  These tests use that
// function to assert behavior MATCHES the build configuration — so a
// CI accidentally building with PYLABHUB_WITH_TEST=ON gets a loud
// failure rather than silently relaxing security.  In RELEASE +
// PYLABHUB_WITH_TEST=OFF (production binaries) the enforce branch
// runs.

TEST(KnownRolesStoreTest, I10_BuildFlag_MatchesNDebugDisposition)
{
    // PYLABHUB_WITH_TEST may be defined regardless of build type, but
    // it has effect only in DEBUG (when NDEBUG is undefined).  Hence:
    //   RELEASE (NDEBUG) → always enforces, irrespective of the flag.
    //   DEBUG + WITH_TEST → bypass.
    //   DEBUG + no WITH_TEST → enforces.
#if defined(NDEBUG) || !defined(PYLABHUB_WITH_TEST)
    EXPECT_TRUE(
        pylabhub::utils::security::known_roles_enforces_unique_pubkey())
        << "RELEASE or DEBUG-without-WITH_TEST builds MUST enforce "
           "HEP-CORE-0036 §I10.  Compile-time symbol drift detected.";
#else
    EXPECT_FALSE(
        pylabhub::utils::security::known_roles_enforces_unique_pubkey())
        << "DEBUG + PYLABHUB_WITH_TEST builds MUST report the bypass "
           "is active.  Compile-time symbol drift detected.";
#endif
}

TEST(KnownRolesStoreTest, I10_Add_DuplicatePubkey_DifferentUid_RejectedOrAllowed)
{
    KnownRolesStore s;
    ASSERT_TRUE(s.add(make_role("uid.alice", fake_pubkey('A'))));
    if (pylabhub::utils::security::known_roles_enforces_unique_pubkey())
    {
        EXPECT_THROW(
            s.add(make_role("uid.bob", fake_pubkey('A'))),
            std::runtime_error)
            << "HEP-CORE-0036 §I10: pubkey already used by another uid "
               "must be rejected.";
        EXPECT_EQ(s.size(), 1u);
        EXPECT_FALSE(s.find("uid.bob").has_value());
    }
    else
    {
        EXPECT_NO_THROW(s.add(make_role("uid.bob", fake_pubkey('A'))))
            << "PYLABHUB_WITH_TEST=ON + DEBUG: shared pubkey across "
               "distinct uids must be allowed (L3 multi-BRC fixture).";
        EXPECT_EQ(s.size(), 2u);
    }
}

TEST(KnownRolesStoreTest, I10_Add_SameUidReplace_PubkeyRotation_AlwaysAllowed)
{
    // Replacement of an existing entry under the SAME uid is allowed
    // unconditionally.  Pubkey rotation use case — the invariant only
    // forbids the same pubkey under DIFFERENT uids.
    KnownRolesStore s;
    ASSERT_TRUE(s.add(make_role("uid.alice", fake_pubkey('A'))));
    EXPECT_NO_THROW(s.add(make_role("uid.alice", fake_pubkey('B'))));
    EXPECT_EQ(s.size(), 1u);
    auto got = s.find("uid.alice");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->pubkey_z85, fake_pubkey('B'));
}

TEST(KnownRolesStoreTest,
     I10_Add_DistinctPubkeyAcrossUids_AlwaysAllowed)
{
    // The happy path — production-mirror shape with one keypair per
    // role uid — must always succeed, regardless of build flag.
    KnownRolesStore s;
    EXPECT_NO_THROW(s.add(make_role("uid.alice", fake_pubkey('A'))));
    EXPECT_NO_THROW(s.add(make_role("uid.bob",   fake_pubkey('B'))));
    EXPECT_NO_THROW(s.add(make_role("uid.carol", fake_pubkey('C'))));
    EXPECT_EQ(s.size(), 3u);
}

TEST(KnownRolesStoreTest,
     I10_LoadFromFile_DuplicatePubkey_RejectedOrAllowed)
{
    // Construct a JSON file directly so the test exercises the
    // post-load validation rather than relying on save_to_file
    // (which goes through `add()` — also enforced, but the file path
    // can carry duplicate pubkeys from an external operator that
    // hand-edited the file).
    auto p = make_tmp_path("i10_load_dup_pubkey");
    const std::string body =
        "{\n"
        "  \"version\": 1,\n"
        "  \"roles\": [\n"
        "    { \"name\": \"r1\", \"uid\": \"uid.alice\", "
                  "\"role\": \"producer\", \"pubkey_z85\": \""
                  + fake_pubkey('A') + "\" },\n"
        "    { \"name\": \"r2\", \"uid\": \"uid.bob\", "
                  "\"role\": \"consumer\", \"pubkey_z85\": \""
                  + fake_pubkey('A') + "\" }\n"
        "  ]\n"
        "}\n";
    atomic_write_owner_only_file(p, body);

    if (pylabhub::utils::security::known_roles_enforces_unique_pubkey())
    {
        EXPECT_THROW(KnownRolesStore::load_from_file(p),
                     std::runtime_error)
            << "HEP-CORE-0036 §I10: file with duplicate pubkey under "
               "different uids must be rejected at load time.";
    }
    else
    {
        std::optional<KnownRolesStore> store;
        EXPECT_NO_THROW(
            store = KnownRolesStore::load_from_file(p));
        ASSERT_TRUE(store.has_value());
        EXPECT_EQ(store->size(), 2u);
    }
    fs::remove(p);
}

TEST(KnownRolesStoreTest,
     I10_SaveLoad_RoundTrip_CompliantStoreSurvives)
{
    // HEP-CORE-0036 §I10 closure pin: an I10-compliant store written
    // by save_to_file MUST be loadable by load_from_file with all
    // entries preserved.  Without this assertion, a future refactor
    // that quietly tightened the on-disk schema (e.g. added a
    // pubkey-uniqueness check that rejected its own output) would
    // produce stores that can't reopen their own files.
    //
    // The opposite direction — load rejecting a file save would
    // accept — is covered by `I10_LoadFromFile_DuplicatePubkey_...`
    // above (in enforce mode, the file with shared pubkey is
    // rejected by load even though it could in principle be written
    // by an external tool).
    auto p = make_tmp_path("i10_save_load_roundtrip");

    KnownRolesStore original;
    ASSERT_TRUE(original.add(make_role("uid.alice", fake_pubkey('A'),
                                       "Alice", "producer")));
    ASSERT_TRUE(original.add(make_role("uid.bob",   fake_pubkey('B'),
                                       "Bob",   "consumer")));
    ASSERT_TRUE(original.add(make_role("uid.carol", fake_pubkey('C'),
                                       "Carol", "processor")));
    ASSERT_EQ(original.size(), 3u);

    EXPECT_NO_THROW(original.save_to_file(p));

    std::optional<KnownRolesStore> reloaded;
    EXPECT_NO_THROW(reloaded = KnownRolesStore::load_from_file(p));
    ASSERT_TRUE(reloaded.has_value())
        << "An I10-compliant store must round-trip through "
           "save_to_file → load_from_file in every build mode.";

    EXPECT_EQ(reloaded->size(), 3u);
    // Insertion-order preservation is checked elsewhere; here pin
    // that every uid + pubkey survived, regardless of order.
    for (const std::string &uid :
         {"uid.alice", "uid.bob", "uid.carol"})
    {
        auto found = reloaded->find(uid);
        ASSERT_TRUE(found.has_value()) << "uid '" << uid << "' lost";
        auto orig = original.find(uid);
        ASSERT_TRUE(orig.has_value());
        EXPECT_EQ(found->pubkey_z85, orig->pubkey_z85);
        EXPECT_EQ(found->name,       orig->name);
        EXPECT_EQ(found->role,       orig->role);
    }
    fs::remove(p);
}

TEST(KnownRolesStoreTest,
     I10_SaveLoad_HandEditedDuplicate_DetectedOnReload)
{
    // HEP-CORE-0036 §I10 operator-attack-surface pin: even a store
    // populated by save_to_file (which never produces duplicates
    // because every add() rejects them) becomes a duplicate-bearing
    // file if an operator hand-edits the JSON.  The reload path
    // MUST detect this — file persistence is the trust boundary for
    // operator intent, and we never silently downgrade.
    //
    // Builds the file in two steps:
    //   1. Save a legitimate single-entry store via save_to_file
    //      (gives us a JSON shape produced by the real serializer).
    //   2. Replace the file with one that duplicates the pubkey
    //      under a second uid (simulates the hand-edit).
    auto p = make_tmp_path("i10_handedit_duplicate");

    KnownRolesStore seed;
    ASSERT_TRUE(seed.add(make_role("uid.alice", fake_pubkey('A'),
                                   "Alice", "producer")));
    seed.save_to_file(p);

    // Hand-edit: introduce a second entry that reuses Alice's
    // pubkey under a different uid.  Use raw JSON so we don't
    // depend on save_to_file's internal helper.
    const std::string tampered_body =
        "{\n"
        "  \"version\": 1,\n"
        "  \"roles\": [\n"
        "    { \"name\": \"Alice\", \"uid\": \"uid.alice\", "
                  "\"role\": \"producer\", \"pubkey_z85\": \""
                  + fake_pubkey('A') + "\" },\n"
        "    { \"name\": \"Mallory\", \"uid\": \"uid.mallory\", "
                  "\"role\": \"producer\", \"pubkey_z85\": \""
                  + fake_pubkey('A') + "\" }\n"
        "  ]\n"
        "}\n";
    atomic_write_owner_only_file(p, tampered_body);

    if (pylabhub::utils::security::known_roles_enforces_unique_pubkey())
    {
        EXPECT_THROW(KnownRolesStore::load_from_file(p),
                     std::runtime_error)
            << "HEP-CORE-0036 §I10: a hand-edited file that creates "
               "(uid.alice, X) + (uid.mallory, X) must be rejected on "
               "reload.  Without this rejection, the broker would "
               "admit Mallory using Alice's keypair after a config "
               "reload — exactly the privilege-confusion failure "
               "mode I10 exists to block.";
    }
    else
    {
        // Bypass mode (DEBUG + WITH_TEST): test fixture path; the
        // bypass exists for L3 multi-BRC scenarios.  We verify the
        // load actually succeeds (otherwise the bypass is broken).
        std::optional<KnownRolesStore> reloaded;
        EXPECT_NO_THROW(
            reloaded = KnownRolesStore::load_from_file(p));
        ASSERT_TRUE(reloaded.has_value());
        EXPECT_EQ(reloaded->size(), 2u);
    }
    fs::remove(p);
}
