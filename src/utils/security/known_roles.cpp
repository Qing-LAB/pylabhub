/**
 * @file known_roles.cpp
 * @brief Implementation of KnownRolesStore (PeerAdmission Phase B).
 */
#include "utils/security/known_roles.hpp"

#include "utils/security/key_file_acl.hpp" // atomic_write_owner_only_file
                                           // verify_keyfile_acl
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio> // std::fprintf for advisory diagnostic
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pylabhub::utils::security
{

// HEP-CORE-0036 §I10 — one-pubkey-per-role-uid invariant.  RELEASE
// builds always enforce; DEBUG + PYLABHUB_WITH_TEST allows the
// fixture bypass for L3 in-process multi-BRC tests.  The constant is
// defined at file scope (not inside an anonymous namespace) so it is
// visible to the L2 test that asserts enforcement matches the build
// configuration — see `tests/test_layer2_service/test_known_roles.cpp`.
#if defined(NDEBUG) || !defined(PYLABHUB_WITH_TEST)
inline constexpr bool kEnforceUniquePubkey = true;
#else
inline constexpr bool kEnforceUniquePubkey = false;
#endif

namespace
{

constexpr std::size_t kZ85PubkeyChars = 40;

/// HEP-CORE-0036 §I10 enforcement helper.  Throws `std::runtime_error`
/// with @p context_label embedded in the message if @p incoming_pubkey
/// already appears in @p existing_roles under a different uid.  A
/// matching uid means the caller is REPLACING an entry, not creating
/// a duplicate — that's allowed (pubkey rotation use case).
///
/// In DEBUG + PYLABHUB_WITH_TEST builds (`kEnforceUniquePubkey ==
/// false`) the body is a no-op — production-time enforcement is
/// physically absent from the compiled binary so it cannot be
/// re-enabled at runtime.
void enforce_unique_pubkey_invariant(const std::vector<broker::KnownRole> &existing_roles,
                                     const std::string &incoming_uid,
                                     const std::string &incoming_pubkey, const char *context_label)
{
    if constexpr (!kEnforceUniquePubkey)
    {
        return;
    }
    for (const auto &existing : existing_roles)
    {
        if (existing.uid == incoming_uid)
            continue; // same-uid replace = pubkey rotation; not a dup
        if (existing.pubkey_z85 == incoming_pubkey)
        {
            std::string msg = "KnownRolesStore: ";
            msg += context_label;
            msg += ": pubkey already in use by uid '";
            msg += existing.uid;
            msg += "' (incoming uid '";
            msg += incoming_uid;
            msg += "').  One pubkey per role uid is required "
                   "(HEP-CORE-0036 §I10 — separation of duties).";
            throw std::runtime_error(msg);
        }
    }
}

void validate_entry(const broker::KnownRole &entry)
{
    if (entry.uid.empty())
        throw std::invalid_argument("KnownRolesStore: refusing to add entry with empty uid");
    if (entry.pubkey_z85.empty())
        throw std::invalid_argument("KnownRolesStore: refusing to add entry with empty "
                                    "pubkey_z85 (uid='" +
                                    entry.uid + "')");
    if (entry.pubkey_z85.size() != kZ85PubkeyChars)
        throw std::invalid_argument("KnownRolesStore: pubkey_z85 must be exactly 40 chars "
                                    "(Z85-encoded CURVE pubkey); got " +
                                    std::to_string(entry.pubkey_z85.size()) + " (uid='" +
                                    entry.uid + "')");
}

nlohmann::json entry_to_json(const broker::KnownRole &e)
{
    nlohmann::json j;
    j["name"] = e.name;
    j["uid"] = e.uid;
    j["role"] = e.role;
    j["pubkey_z85"] = e.pubkey_z85;
    return j;
}

/// Strict typed extraction.  `nlohmann::json::value(k, default)`
/// returns the default both when @p field is absent AND when it is
/// present with the wrong type — defeating the strict-parser intent
/// of `KnownRolesStore`.  Concrete bug surfaced by the fresh-eye
/// review: `"pubkey_z85": null` and `"name": 1234` were silently
/// coerced to `""`, breaking save→load round-trip identity and
/// hiding operator-edit mistakes.
///
/// Contract here:
///   - missing or JSON-null → empty string (legacy entries that
///     omit optional fields like `name`/`role` still load — the
///     final validate_entry catches the empty-uid/empty-pubkey case)
///   - present but wrong type → throws std::runtime_error naming the
///     field and the observed JSON type
std::string require_string_or_empty(const nlohmann::json &j, const char *field)
{
    auto it = j.find(field);
    if (it == j.end() || it->is_null())
        return {};
    if (!it->is_string())
        throw std::runtime_error("KnownRolesStore: field '" + std::string(field) +
                                 "' must be a string; got JSON type '" +
                                 std::string(it->type_name()) + "'");
    return it->get<std::string>();
}

broker::KnownRole entry_from_json(const nlohmann::json &j)
{
    broker::KnownRole e;
    e.name = require_string_or_empty(j, "name");
    e.uid = require_string_or_empty(j, "uid");
    e.role = require_string_or_empty(j, "role");
    e.pubkey_z85 = require_string_or_empty(j, "pubkey_z85");
    return e;
}

} // namespace

// ── KnownRolesStore ─────────────────────────────────────────────────────────

std::optional<KnownRolesStore> KnownRolesStore::load_from_file(const std::filesystem::path &path)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(path, ec) || ec)
        return std::nullopt;

    // ACL check before reading — the file dictates who's admitted;
    // a tampered or world-readable known_roles.json must NOT be
    // honored.  Mirrors the HubConfig::load_keypair / RoleConfig::
    // load_keypair pattern from #101.
    //
    // **H-K2 fix.**  The verify_keyfile_acl contract (key_file_acl.hpp
    // §"AclVerdict" docs) requires callers to gate emission on
    // `!diagnostic.empty()`, NOT just `!ok`.  When the parent
    // directory is group/world-accessible, `verify_vault_file`
    // appends an advisory diagnostic to `v.diagnostic` but keeps
    // `v.ok = true` (per HEP-CORE-0035 §4.6.2 — shared-host setups
    // sometimes legitimately want group-readable parents).  The old
    // code only acted on `!v.ok`, silently swallowing that warning.
    // Now we surface the advisory via stderr (matching the existing
    // RoleConfig::load advisory pattern) so the operator sees the
    // leak signal during audit.
    {
        auto v = verify_keyfile_acl(path, KeyFileRole::VaultFile);
        if (!v.ok)
            throw std::runtime_error("KnownRolesStore: refusing to load '" + path.string() +
                                     "' — ACL check failed (HEP-CORE-0035 §4.6.2):\n" +
                                     v.diagnostic);
        if (!v.diagnostic.empty())
            std::fprintf(stderr,
                         "[KnownRolesStore] WARN: ACL advisory for "
                         "'%s' (HEP-CORE-0035 §4.6.2):\n%s\n",
                         path.string().c_str(), v.diagnostic.c_str());
    }

    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("KnownRolesStore: cannot open '" + path.string() + "'");

    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const nlohmann::json::exception &ex)
    {
        throw std::runtime_error("KnownRolesStore: JSON parse failed for '" + path.string() +
                                 "': " + ex.what());
    }

    // File I/O done; strict parse + validation is medium-agnostic.
    return from_json(j, path.string());
}

// ── JSON codec (HEP-CORE-0035 §4.8) — one model, two storage media ──────────

KnownRolesStore KnownRolesStore::from_json(const nlohmann::json &j, std::string_view context)
{
    const std::string ctx(context);

    if (!j.contains("version") || !j["version"].is_number_integer())
        throw std::runtime_error("KnownRolesStore: '" + ctx + "' missing integer 'version' field");
    const int version = j["version"].get<int>();
    if (version != kKnownRolesSchemaVersion)
        throw std::runtime_error("KnownRolesStore: '" + ctx + "' has unknown schema version " +
                                 std::to_string(version) + " (this build understands version " +
                                 std::to_string(kKnownRolesSchemaVersion) + ")");

    KnownRolesStore store;
    if (j.contains("roles"))
    {
        if (!j["roles"].is_array())
            throw std::runtime_error("KnownRolesStore: '" + ctx +
                                     "' has 'roles' that is not an array");
        for (const auto &je : j["roles"])
        {
            auto e = entry_from_json(je);
            validate_entry(e);
            // Reject duplicate uid — operator ambiguity should surface,
            // not silently dedupe.
            for (const auto &existing : store.roles_)
            {
                if (existing.uid == e.uid)
                    throw std::runtime_error("KnownRolesStore: '" + ctx + "' has duplicate uid '" +
                                             e.uid + "'");
            }
            // HEP-CORE-0036 §I10: reject shared pubkey across distinct
            // uids.  Strict rejection so operator intent is never
            // silently downgraded.  In DEBUG + PYLABHUB_WITH_TEST builds
            // the check is a no-op (fixture bypass).
            enforce_unique_pubkey_invariant(store.roles_, e.uid, e.pubkey_z85,
                                            ("from_json('" + ctx + "')").c_str());
            store.roles_.push_back(std::move(e));
        }
    }
    return store;
}

nlohmann::json KnownRolesStore::to_json() const
{
    nlohmann::json j;
    j["version"] = kKnownRolesSchemaVersion;
    j["roles"] = nlohmann::json::array();
    for (const auto &e : roles_)
        j["roles"].push_back(entry_to_json(e));
    return j;
}

void KnownRolesStore::save_to_file(const std::filesystem::path &path) const
{
    // Pretty-print for operator-readable diffs.  Compact JSON would
    // save bytes but operators occasionally `cat`/`git diff` this
    // file when auditing.
    const std::string serialized = to_json().dump(/*indent=*/2);
    atomic_write_owner_only_file(path, serialized);
}

bool KnownRolesStore::add(broker::KnownRole entry)
{
    validate_entry(entry);
    // HEP-CORE-0036 §I10: reject duplicate pubkey under a DIFFERENT
    // uid before any state mutation.  Replacement of the same uid
    // (pubkey rotation) is allowed by `enforce_unique_pubkey_invariant`.
    enforce_unique_pubkey_invariant(roles_, entry.uid, entry.pubkey_z85, "add()");
    auto it = std::find_if(roles_.begin(), roles_.end(),
                           [&](const broker::KnownRole &e) { return e.uid == entry.uid; });
    if (it != roles_.end())
    {
        *it = std::move(entry);
        return false; // replaced
    }
    roles_.push_back(std::move(entry));
    return true; // inserted
}

bool KnownRolesStore::remove(const std::string &uid)
{
    auto it = std::find_if(roles_.begin(), roles_.end(),
                           [&](const broker::KnownRole &e) { return e.uid == uid; });
    if (it == roles_.end())
        return false;
    roles_.erase(it);
    return true;
}

std::optional<broker::KnownRole> KnownRolesStore::find(const std::string &uid) const
{
    auto it = std::find_if(roles_.begin(), roles_.end(),
                           [&](const broker::KnownRole &e) { return e.uid == uid; });
    if (it == roles_.end())
        return std::nullopt;
    return *it;
}

std::optional<broker::KnownRole>
KnownRolesStore::find_by_pubkey(const std::string &pubkey_z85) const
{
    auto it = std::find_if(roles_.begin(), roles_.end(),
                           [&](const broker::KnownRole &e) { return e.pubkey_z85 == pubkey_z85; });
    if (it == roles_.end())
        return std::nullopt;
    return *it;
}

const std::vector<broker::KnownRole> &KnownRolesStore::list() const noexcept
{
    return roles_;
}

std::size_t KnownRolesStore::size() const noexcept
{
    return roles_.size();
}

bool KnownRolesStore::empty() const noexcept
{
    return roles_.empty();
}

bool known_roles_enforces_unique_pubkey() noexcept
{
    return kEnforceUniquePubkey;
}

PeerAllowlist KnownRolesStore::as_peer_allowlist() const
{
    PeerAllowlist al;
    for (const auto &e : roles_)
    {
        // `validate_entry` (the mandatory gate on both insertion paths,
        // `from_json` + `add`) rejects empty/short pubkeys, so every
        // stored entry carries a valid 40-char pubkey.  The former
        // empty-pubkey skip here was dead once known_roles moved into
        // the vault (HEP-0035 §4.8); the legacy string gate it deferred
        // to is deleted (§4.5 / §8 Phase 6).
        assert(!e.pubkey_z85.empty() && "KnownRole with empty pubkey reached the store");
        al.peers.insert(PeerIdentity{"curve", e.pubkey_z85});
    }
    return al;
}

} // namespace pylabhub::utils::security
