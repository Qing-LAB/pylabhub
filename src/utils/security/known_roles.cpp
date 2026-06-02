/**
 * @file known_roles.cpp
 * @brief Implementation of KnownRolesStore (PeerAdmission Phase B).
 */
#include "utils/security/known_roles.hpp"

#include "utils/security/key_file_acl.hpp"   // atomic_write_owner_only_file
                                              // verify_keyfile_acl
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>     // std::fprintf for advisory diagnostic
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pylabhub::utils::security
{

namespace
{

constexpr std::size_t kZ85PubkeyChars = 40;

void validate_entry(const broker::KnownRole &entry)
{
    if (entry.uid.empty())
        throw std::invalid_argument(
            "KnownRolesStore: refusing to add entry with empty uid");
    if (entry.pubkey_z85.empty())
        throw std::invalid_argument(
            "KnownRolesStore: refusing to add entry with empty "
            "pubkey_z85 (uid='" + entry.uid + "')");
    if (entry.pubkey_z85.size() != kZ85PubkeyChars)
        throw std::invalid_argument(
            "KnownRolesStore: pubkey_z85 must be exactly 40 chars "
            "(Z85-encoded CURVE pubkey); got " +
            std::to_string(entry.pubkey_z85.size()) +
            " (uid='" + entry.uid + "')");
}

nlohmann::json entry_to_json(const broker::KnownRole &e)
{
    nlohmann::json j;
    j["name"]       = e.name;
    j["uid"]        = e.uid;
    j["role"]       = e.role;
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
std::string require_string_or_empty(const nlohmann::json &j,
                                     const char *field)
{
    auto it = j.find(field);
    if (it == j.end() || it->is_null())
        return {};
    if (!it->is_string())
        throw std::runtime_error(
            "KnownRolesStore: field '" + std::string(field) +
            "' must be a string; got JSON type '" +
            std::string(it->type_name()) + "'");
    return it->get<std::string>();
}

broker::KnownRole entry_from_json(const nlohmann::json &j)
{
    broker::KnownRole e;
    e.name       = require_string_or_empty(j, "name");
    e.uid        = require_string_or_empty(j, "uid");
    e.role       = require_string_or_empty(j, "role");
    e.pubkey_z85 = require_string_or_empty(j, "pubkey_z85");
    return e;
}

} // namespace

// ── KnownRolesStore ─────────────────────────────────────────────────────────

std::optional<KnownRolesStore>
KnownRolesStore::load_from_file(const std::filesystem::path &path)
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
            throw std::runtime_error(
                "KnownRolesStore: refusing to load '" +
                path.string() +
                "' — ACL check failed (HEP-CORE-0035 §4.6.2):\n" +
                v.diagnostic);
        if (!v.diagnostic.empty())
            std::fprintf(stderr,
                         "[KnownRolesStore] WARN: ACL advisory for "
                         "'%s' (HEP-CORE-0035 §4.6.2):\n%s\n",
                         path.string().c_str(),
                         v.diagnostic.c_str());
    }

    std::ifstream f(path);
    if (!f)
        throw std::runtime_error(
            "KnownRolesStore: cannot open '" + path.string() + "'");

    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const nlohmann::json::exception &ex)
    {
        throw std::runtime_error(
            "KnownRolesStore: JSON parse failed for '" + path.string() +
            "': " + ex.what());
    }

    if (!j.contains("version") || !j["version"].is_number_integer())
        throw std::runtime_error(
            "KnownRolesStore: '" + path.string() +
            "' missing integer 'version' field");
    const int version = j["version"].get<int>();
    if (version != kKnownRolesSchemaVersion)
        throw std::runtime_error(
            "KnownRolesStore: '" + path.string() + "' has unknown "
            "schema version " + std::to_string(version) +
            " (this build understands version " +
            std::to_string(kKnownRolesSchemaVersion) + ")");

    KnownRolesStore store;
    if (j.contains("roles"))
    {
        if (!j["roles"].is_array())
            throw std::runtime_error(
                "KnownRolesStore: '" + path.string() +
                "' has 'roles' that is not an array");
        for (const auto &je : j["roles"])
        {
            auto e = entry_from_json(je);
            validate_entry(e);
            // Reject duplicate uid in the on-disk file — operator
            // ambiguity should surface, not silently dedupe.
            for (const auto &existing : store.roles_)
            {
                if (existing.uid == e.uid)
                    throw std::runtime_error(
                        "KnownRolesStore: '" + path.string() +
                        "' has duplicate uid '" + e.uid + "'");
            }
            store.roles_.push_back(std::move(e));
        }
    }
    return store;
}

void KnownRolesStore::save_to_file(const std::filesystem::path &path) const
{
    nlohmann::json j;
    j["version"] = kKnownRolesSchemaVersion;
    j["roles"]   = nlohmann::json::array();
    for (const auto &e : roles_)
        j["roles"].push_back(entry_to_json(e));

    // Pretty-print for operator-readable diffs.  Compact JSON would
    // save bytes but operators occasionally `cat`/`git diff` this
    // file when auditing.
    const std::string serialized = j.dump(/*indent=*/2);
    atomic_write_owner_only_file(path, serialized);
}

bool KnownRolesStore::add(broker::KnownRole entry)
{
    validate_entry(entry);
    auto it = std::find_if(roles_.begin(), roles_.end(),
                           [&](const broker::KnownRole &e) {
                               return e.uid == entry.uid;
                           });
    if (it != roles_.end())
    {
        *it = std::move(entry);
        return false;  // replaced
    }
    roles_.push_back(std::move(entry));
    return true;  // inserted
}

bool KnownRolesStore::remove(const std::string &uid)
{
    auto it = std::find_if(roles_.begin(), roles_.end(),
                           [&](const broker::KnownRole &e) {
                               return e.uid == uid;
                           });
    if (it == roles_.end())
        return false;
    roles_.erase(it);
    return true;
}

std::optional<broker::KnownRole>
KnownRolesStore::find(const std::string &uid) const
{
    auto it = std::find_if(roles_.begin(), roles_.end(),
                           [&](const broker::KnownRole &e) {
                               return e.uid == uid;
                           });
    if (it == roles_.end())
        return std::nullopt;
    return *it;
}

std::optional<broker::KnownRole>
KnownRolesStore::find_by_pubkey(const std::string &pubkey_z85) const
{
    auto it = std::find_if(roles_.begin(), roles_.end(),
                           [&](const broker::KnownRole &e) {
                               return e.pubkey_z85 == pubkey_z85;
                           });
    if (it == roles_.end())
        return std::nullopt;
    return *it;
}

const std::vector<broker::KnownRole> &
KnownRolesStore::list() const noexcept
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

PeerAllowlist KnownRolesStore::as_peer_allowlist() const
{
    PeerAllowlist al;
    for (const auto &e : roles_)
    {
        if (!e.pubkey_z85.empty())
            al.peers.insert(PeerIdentity{"curve", e.pubkey_z85});
    }
    return al;
}

} // namespace pylabhub::utils::security
