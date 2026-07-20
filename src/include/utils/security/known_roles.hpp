#pragma once
/**
 * @file known_roles.hpp
 * @brief Operator-managed allowlist of authorized roles, keyed on
 *        CURVE pubkey.  PeerAdmission Phase B — feeds the broker
 *        ROUTER ZAP handler (Phase C+) and the per-channel
 *        ChannelAccessIndex computations (Phase D).
 *
 * **Storage.** Persisted at `<hub_dir>/vault/known_roles.json`,
 * file mode 0600 (operator-tamper-resistant), parent dir 0700
 * (enforced by HEP-CORE-0035 §4.6 / #101 utility).  Per design doc
 * §8 S5 decision (b): pubkeys are NOT secret material, so encryption
 * adds no value vs. the file-ACL floor — but integrity matters
 * absolutely (an attacker who modifies this file controls who can
 * connect).
 *
 * **File format.**
 * ```json
 * {
 *   "version": 1,
 *   "roles": [
 *     {
 *       "name":       "lab.daq.sensor1",
 *       "uid":        "prod.sensor.uid12345678",
 *       "role":       "producer",
 *       "pubkey_z85": "<40-char Z85>"
 *     }
 *   ]
 * }
 * ```
 *
 * Future versions add fields by bumping `version`.  The reader
 * tolerates unknown top-level keys (forward compat) but rejects
 * unknown `version` numbers (catches schema drift).
 *
 * **Mutation semantics.** `add` upserts by `uid` (replacing any prior
 * entry with the same uid — pubkey rotation use case).  `remove`
 * deletes by `uid`.  All persistent state changes go through
 * `save_to_file` which uses
 * `pylabhub::utils::security::atomic_write_owner_only_file` (atomic
 * O_CREAT|O_EXCL|O_NOFOLLOW tmp + rename(2)).  Either the new state
 * is wholly on disk, or the prior state survives unmodified.
 *
 * **Bridge to PeerAdmission (Phase A).** `as_peer_allowlist()`
 * projects the store onto a `PeerAllowlist` containing exactly the
 * entries whose `pubkey_z85` is non-empty.  This is what the broker
 * ROUTER's `PeerAdmission::set_peer_allowlist` receives.  Legacy
 * entries (pre-Phase-B vault contents with empty pubkey) are
 * EXCLUDED from the allowlist — the legacy `RoleIdentityPolicy`
 * string-match check (`broker_service.cpp::check_role_identity`)
 * still handles them at the application layer until HEP-0035 §8
 * Phase 6 retires it.
 *
 * **Thread-safety.** The class itself is NOT thread-safe.  Callers
 * serialize access (broker single-threaded; CLI ops run pre-startup).
 */

#include "pylabhub_utils_export.h"

#include "utils/json_fwd.hpp"                // nlohmann::json (fwd)
#include "utils/role_identity_policy.hpp"   // ::pylabhub::broker::KnownRole
#include "utils/security/peer_admission.hpp" // PeerAllowlist

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pylabhub::utils::security
{

/// Current schema version written by save_to_file().
inline constexpr int kKnownRolesSchemaVersion = 1;

/// HEP-CORE-0036 §I10 — compile-time disposition of the
/// "one pubkey per role uid" security invariant.
///
/// `true` in RELEASE builds and in DEBUG builds compiled WITHOUT
/// `PYLABHUB_WITH_TEST` — `KnownRolesStore::add()` and
/// `::load_from_file()` reject any configuration where two distinct
/// uids share a pubkey.
///
/// `false` ONLY when compiled with `CMAKE_BUILD_TYPE=Debug` AND the
/// CMake option `PYLABHUB_WITH_TEST=ON` — the bypass exists for L3
/// in-process multi-BRC test fixtures (HEP-CORE-0036 §I10 WARNING
/// box).  RELEASE builds physically lack the bypass code path; this
/// constant is `true` in every shippable binary.
///
/// L2 tests assert behavior matches this constant (see
/// `tests/test_layer2_service/test_known_roles_store.cpp` for the
/// pin) so a misconfigured CI cannot silently drift between the two
/// modes.
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool
known_roles_enforces_unique_pubkey() noexcept;

/// Operator-managed allowlist of authorized roles, keyed on CURVE
/// pubkey.  See file-header docs for the on-disk schema + the bridge
/// to `PeerAllowlist` consumed by the broker's PeerAdmission gate.
class PYLABHUB_UTILS_EXPORT KnownRolesStore
{
public:
    /// Construct an empty store (no entries).
    KnownRolesStore() = default;

    /// Load from @p path.  Returns:
    ///   - Populated store on successful parse (including version-1
    ///     files with zero or more entries).
    ///   - Empty std::nullopt if the file does NOT exist (caller
    ///     typically logs a WARN and continues with deny-all per
    ///     design §8 S6 P-Bootstrap recommendation b).
    /// Throws std::runtime_error on:
    ///   - File exists but ACL check fails (mode/owner wrong).
    ///   - File exists but JSON parse fails.
    ///   - `version` field absent or unknown.
    ///   - Any entry fails validation (empty uid / pubkey of wrong
    ///     length / etc.).
    ///
    /// Validation is strict: a bad file at startup is a hard failure
    /// rather than a "skip bad entries and continue", because the
    /// operator's auth intent must not be silently downgraded.
    [[nodiscard]] static std::optional<KnownRolesStore>
    load_from_file(const std::filesystem::path &path);

    /// Save the current state to @p path using
    /// `atomic_write_owner_only_file`.  Throws on I/O failure.  The
    /// pre-call file content survives any failure mode (atomic
    /// rename either completes or doesn't happen at all).
    ///
    /// NOTE: file storage is retained for one-shot migration
    /// (`--migrate-known-roles`) and tests only.  The authoritative
    /// allowlist home is the encrypted hub vault (HEP-CORE-0035 §4.8);
    /// production load/mutate goes through `from_json`/`to_json` against
    /// the decrypted vault payload, NOT a standalone file.
    void save_to_file(const std::filesystem::path &path) const;

    /// Medium-agnostic JSON codec (HEP-CORE-0035 §4.8).  `to_json`
    /// emits the `{version, roles:[…]}` document; `from_json` parses
    /// and validates it with the SAME strict rules as `load_from_file`
    /// (unknown `version` rejected, per-entry validation, duplicate-uid
    /// and shared-pubkey rejection).  These are the primitives the
    /// hub vault (`HubVault::known_roles` / `set_known_roles`) and the
    /// file I/O both build on — one model, two storage media.
    ///
    /// @param context  Human-readable source label woven into error
    ///                 messages (e.g. a vault path or `"hub vault"`).
    /// @throws std::runtime_error on malformed/invalid input.
    [[nodiscard]] ::nlohmann::json to_json() const;
    [[nodiscard]] static KnownRolesStore
    from_json(const ::nlohmann::json &j, std::string_view context);

    /// Insert or replace an entry keyed on `uid`.  Returns true iff
    /// the entry was newly inserted (false → replaced an existing
    /// entry with the same uid).
    ///
    /// Validation: throws std::invalid_argument if:
    ///   - `uid` is empty
    ///   - `pubkey_z85` is empty OR not exactly 40 chars
    /// Other fields (`name`, `role`) are permissive — operator-facing
    /// strings the policy layer does not enforce.
    bool add(::pylabhub::broker::KnownRole entry);

    /// Remove the entry with the given uid.  Returns true iff an
    /// entry was removed (false → no such uid).
    bool remove(const std::string &uid);

    /// Look up by uid.
    [[nodiscard]] std::optional<::pylabhub::broker::KnownRole>
    find(const std::string &uid) const;

    /// Lookup by pubkey.  O(N) — pubkey is the secondary index that
    /// the ZAP handler queries; for the broker the bridge through
    /// `as_peer_allowlist()` is the preferred path (set membership
    /// O(log N) after one `as_peer_allowlist()` snapshot is taken).
    [[nodiscard]] std::optional<::pylabhub::broker::KnownRole>
    find_by_pubkey(const std::string &pubkey_z85) const;

    /// All entries, in insertion order.  Used by `--list-known-roles`
    /// CLI for tabular output.
    [[nodiscard]] const std::vector<::pylabhub::broker::KnownRole> &list() const noexcept;

    /// Number of entries.
    [[nodiscard]] std::size_t size() const noexcept;

    /// True iff no entries are stored.
    [[nodiscard]] bool empty() const noexcept;

    /// Project the store onto a `PeerAllowlist` of {"curve", pubkey}
    /// identities.  Entries with empty `pubkey_z85` (legacy migration
    /// remnants) are EXCLUDED — they're handled at the application
    /// layer by the legacy `RoleIdentityPolicy` string check until
    /// HEP-0035 §8 Phase 6 retires it.
    ///
    /// The returned allowlist's `unrestricted` is always false; only
    /// the explicit `--allow-anonymous-data` operator flag (Phase H)
    /// produces an unrestricted allowlist, and that path doesn't
    /// flow through this method.
    [[nodiscard]] PeerAllowlist as_peer_allowlist() const;

private:
    /// In-memory state.  Vector preserves insertion order (matters
    /// for `--list-known-roles` deterministic output) at the cost of
    /// O(N) lookup.  Allowlists are operator-managed and expected to
    /// be small (~tens of entries); the constant is small enough that
    /// std::vector beats a map by avoiding allocation churn on add.
    std::vector<::pylabhub::broker::KnownRole> roles_;
};

} // namespace pylabhub::utils::security
