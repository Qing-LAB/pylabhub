#pragma once
/**
 * @file naming.hpp
 * @brief Hub identifier validators + parsers.
 *
 * Single enforcement point for the identifier grammar ratified in
 * HEP-CORE-0033 Appendix §G2.2.0b "Naming grammar for hub identifiers"
 * (`docs/HEP/HEP-CORE-0033-Hub-Character.md`).
 *
 * HEP is authoritative — this header must match the spec exactly;
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
 * schema     := '$' NameComponent ('.' NameComponent)* '.' 'v' [0-9]+
 *               // ≥2 components after the sigil; last component is the
 *               // version, must match `v<digits>` literally (e.g. v1, v42).
 *               // Examples: "$foo.v1", "$lab.sensors.temp.v42".
 *
 * Numeric-token convention (not validator-enforced; see HEP-0033
 * §G2.2.0b "Numeric-token prefix convention"):
 *   - PID-derived component       → `pid<digits>`      (e.g. "pid104577")
 *   - Random/UUID-style unique     → `uid<hex>`         (e.g. "uid3a7f2b1c")
 *   - Schema version (validator-enforced) → `v<digits>`  (e.g. "v2")
 *
 * Every `NameComponent` must start with `[A-Za-z]`, so bare numeric
 * tokens (`42`, `3a7f2b1c`) fail the grammar.  The conventions above
 * are the project-wide full-word prefixes that make the purpose
 * self-evident in logs.  Prefer the helpers in `uid_utils.hpp` for
 * construction and `parse_role_uid()` / `parse_schema_id()` here for
 * dissection rather than ad-hoc string manipulation.
 * sys.key    := 'sys' ('.' NameComponent)+              // broker-internal
 * NameComponent := [A-Za-z][A-Za-z0-9_-]{0,63}
 * ```
 *
 * Total length ≤ 256 chars; per-component length ≤ 64.
 *
 * The total-length cap is per single identifier (DoS defense at the
 * parsing boundary).  Composite / federated references (e.g. "role X
 * on hub Y") use structured multi-field encoding, not string
 * concatenation, and are not subject to this cap.
 */

#include "pylabhub_utils_export.h"

#include <cstdint>
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

/// Dissection of a well-formed `schema` id (HEP-0033 §G2.2.0b).
/// The trailing version component is factored out so callers don't
/// have to parse it themselves.
struct SchemaIdParts
{
    std::string_view base;            ///< Everything between '$' and the final version component.
    std::string_view version_token;   ///< The literal "v<digits>" tail (e.g. "v2").
    std::uint32_t    version{0};      ///< Parsed integer version (e.g. 2).
};

/**
 * @brief Parse a schema id into its base + version parts.
 *
 * Returns std::nullopt if @p id is not a valid Schema identifier
 * (missing '$' sigil, no '.v<digits>' tail, version digits out of
 * uint32 range, or any component fails NameComponent rules).
 *
 * Views into the caller-owned string; output lifetime is tied to @p id.
 */
[[nodiscard]] PYLABHUB_UTILS_EXPORT
std::optional<SchemaIdParts> parse_schema_id(std::string_view id) noexcept;

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
