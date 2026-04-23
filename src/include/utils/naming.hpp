#pragma once
/**
 * @file naming.hpp
 * @brief Hub identifier validators + parsers.
 *
 * Single enforcement point for the identifier grammar ratified in
 * `docs/tech_draft/HUB_CHARACTER_PREREQUISITES.md` §G2.2.0b "Naming
 * conventions for hub identifiers."
 *
 * Doc is authoritative — this header must match the spec exactly;
 * when inconsistency is detected anywhere (code, tests, HEP examples),
 * it's the code/tests that bend to the spec, not the spec to them.
 *
 * ### Grammar summary (full detail in the HEP addendum)
 *
 * ```
 * channel    := NameComponent ('.' NameComponent)*
 *               // first component NOT in {prod, cons, proc, hub, sys}
 * band       := '!' NameComponent ('.' NameComponent)*
 * role.uid   := ('prod'|'cons'|'proc') '.' NameComponent '.' NameComponent
 *               ('.' NameComponent)*   // ≥3 components
 * role.name  := NameComponent ('.' NameComponent)*      // plain; tag optional
 * peer.uid   := 'hub' '.' NameComponent '.' NameComponent
 *               ('.' NameComponent)*   // same tag.name.unique shape as role.uid
 * schema     := '$' NameComponent ('.' NameComponent)*
 *               // version expressed as a trailing name component, e.g. $foo.v2
 * sys.key    := 'sys' ('.' NameComponent)+              // broker-internal
 * NameComponent := [A-Za-z][A-Za-z0-9_-]{0,63}
 * ```
 *
 * Total length ≤ 128 chars; per-component length ≤ 64.
 */

#include "pylabhub_utils_export.h"

#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::hub
{

/// Identifier kinds in hub state (HEP-0033 §G2.2.0b).
enum class IdentifierKind
{
    Channel,  ///< Plain dotted identifier; first component not in reserved set.
    Band,     ///< Leading sigil '!'.
    RoleUid,  ///< Tag prefix (prod|cons|proc) + name + unique_suffix; ≥3 components.
    RoleName, ///< Plain dotted display name; tag optional. Paired with role_tag in output.
    PeerUid,  ///< Tag prefix 'hub' + name + unique_suffix; ≥3 components (same shape as RoleUid).
    Schema,   ///< Leading sigil '$'.
    SysKey,   ///< Reserved `sys.` broker-internal keys (counters, event types).
};

/// Human-readable label for the kind — used in panic messages and logs.
PYLABHUB_UTILS_EXPORT const char *to_string(IdentifierKind k) noexcept;

/**
 * @brief Validate an identifier string against the grammar for @p kind.
 *
 * Thread-safe, noexcept, no allocations.  Does NOT normalize or
 * transform the input; returns the bare boolean verdict.
 *
 * Callers in production paths (HubState `_on_*` ops) use this + early
 * return + counter bump as the silent-drop strong boundary.  Callers at
 * trusted boundaries (internal asserts, test-helper contracts) use
 * @ref require_valid_identifier to panic on failure.
 */
[[nodiscard]] PYLABHUB_UTILS_EXPORT
bool is_valid_identifier(std::string_view s, IdentifierKind kind) noexcept;

/**
 * @brief `is_valid_identifier`, plus panic on failure.
 *
 * Intended for trusted-input assertions — internal code calling HubState
 * primitives, test fixtures that must have well-formed inputs.  Do NOT
 * use on wire input or script input; those go through silent-drop at
 * the op-entry boundary instead.
 *
 * @param context  Short phrase identifying the caller site (appears in
 *                 the panic message so operators can triage).
 */
PYLABHUB_UTILS_EXPORT
void require_valid_identifier(std::string_view s, IdentifierKind kind,
                              std::string_view context);

/// Dissection of a well-formed tagged uid (role or peer — HEP-0033
/// §G2.2.0b "UID construction").  Both role.uid and peer.uid share
/// the same `tag.name.unique` shape; only the allowed tag set differs.
struct TaggedUidParts
{
    std::string_view tag;    ///< "prod" / "cons" / "proc" (role) or "hub" (peer).
    std::string_view name;   ///< Second dot component.
    std::string_view unique; ///< Third and onward, joined; may contain '.'.
};

/**
 * @brief Parse a role uid into its three mandatory parts.
 *
 * Returns std::nullopt if @p uid is not a valid `RoleUid` (tag must be
 * one of `prod`/`cons`/`proc`).  Views into the caller-owned string;
 * output lifetime is tied to @p uid.
 */
[[nodiscard]] PYLABHUB_UTILS_EXPORT
std::optional<TaggedUidParts> parse_role_uid(std::string_view uid) noexcept;

/**
 * @brief Parse a peer uid into its three mandatory parts.
 *
 * Returns std::nullopt if @p uid is not a valid `PeerUid` (tag must be
 * exactly `hub`).  Same structural rules as role uid.
 */
[[nodiscard]] PYLABHUB_UTILS_EXPORT
std::optional<TaggedUidParts> parse_peer_uid(std::string_view uid) noexcept;

/**
 * @brief Shorthand: the role_tag embedded in a well-formed role uid.
 *
 * @return "prod" / "cons" / "proc" if @p uid is a valid RoleUid,
 *         std::nullopt otherwise.
 */
[[nodiscard]] PYLABHUB_UTILS_EXPORT
std::optional<std::string_view> extract_role_tag(std::string_view uid) noexcept;

/**
 * @brief Canonical human-readable reference to a role.
 *
 * - If @p uid is non-empty, returns it verbatim (self-describing per the
 *   UID construction rule — tag/name/unique all embedded).
 * - Otherwise, falls back to `[<tag>] <name>` with either field
 *   omitted if empty.  Intended for log / admin output where `uid`
 *   may or may not be available.
 */
[[nodiscard]] PYLABHUB_UTILS_EXPORT
std::string format_role_ref(std::string_view uid,
                            std::string_view name = {},
                            std::string_view tag  = {});

} // namespace pylabhub::hub
