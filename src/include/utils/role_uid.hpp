#pragma once
/**
 * @file role_uid.hpp
 * @brief Strictly-validated role uid construction (HEP-CORE-0033 §G2.2.0b).
 *
 * Single-direction helper that composes a role uid from typed parts
 * and validates the result via `pylabhub::hub::is_valid_identifier`
 * (the same predicate the broker uses at the wire boundary).  Use
 * this anywhere a role uid is assembled programmatically —
 * production role configs, demo manifests, CLI tooling, test workers
 * and fixtures — to prevent the "two-component uid silently rejected
 * by the broker as INVALID_REQUEST" footgun (caught 2026-06-14
 * during Pattern 4 rung 2 bring-up).
 *
 * The helper is header-only: composition is `fmt::format` + bounded
 * validation, and inlining it lets every call site benefit from the
 * compile-time string check (the surrounding code is hot only on
 * startup so format/validate per call is cheap).
 *
 * NOT a replacement for the broker-side `validate_identity_fields`
 * gate.  The broker remains the authoritative wire-boundary
 * enforcer; this helper just makes it harder for trusted call sites
 * to construct an invalid uid in the first place.
 */

#include "utils/naming.hpp"        // is_valid_identifier, IdentifierKind
#include "utils/role_presence.hpp" // RoleKind (Producer | Consumer)

#include <fmt/format.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pylabhub::scripting
{

/// All three role-uid tag values per HEP-CORE-0033 §G2.2.0b.
///
/// Separate from `RoleKind` (per-presence runtime role, has only
/// `Producer`/`Consumer` because a processor role holds TWO presences,
/// one of each kind; see `role_presence.hpp:50-60`).  The uid-level
/// tagging adds `proc` for the processor role-host shell — even
/// though processor presences themselves are tagged `prod` or
/// `cons` for routing, the role uid is `proc.<name>.<unique>`.
enum class RoleUidTag : std::uint8_t
{
    Producer,  ///< wire tag "prod"
    Consumer,  ///< wire tag "cons"
    Processor, ///< wire tag "proc"
};

[[nodiscard]] inline constexpr std::string_view tag_string(RoleUidTag t) noexcept
{
    switch (t)
    {
    case RoleUidTag::Producer:
        return "prod";
    case RoleUidTag::Consumer:
        return "cons";
    case RoleUidTag::Processor:
        return "proc";
    }
    return {}; // unreachable; silences compiler
}

/// Map the runtime `RoleKind` (per-presence: Producer or Consumer) to
/// its uid-tag form.  Use this when you have a `RoleKind` in hand and
/// want to compose a uid for that kind directly.  Note that processor
/// roles cannot be expressed via `RoleKind`; use `RoleUidTag::Processor`
/// explicitly with the three-arg `make_role_uid` overload.
[[nodiscard]] inline constexpr RoleUidTag to_uid_tag(RoleKind k) noexcept
{
    return (k == RoleKind::Producer) ? RoleUidTag::Producer : RoleUidTag::Consumer;
}

/**
 * @brief Compose a role uid satisfying HEP-CORE-0033 §G2.2.0b grammar.
 *
 * Layout: `<tag>.<name>.<unique>`.  Composition is `fmt::format`;
 * validation is `pylabhub::hub::is_valid_identifier(uid,
 * IdentifierKind::RoleUid)` — the SAME predicate the broker calls at
 * the wire boundary via `validate_identity_fields`.  Single source of
 * truth; if the grammar evolves, both call sites pick up the change.
 *
 * @throws std::invalid_argument if the composed uid fails grammar
 *         validation, with a diagnostic identifying the offending
 *         (tag, name, unique) triple.
 */
[[nodiscard]] inline std::string make_role_uid(RoleUidTag tag, std::string_view name,
                                               std::string_view unique)
{
    std::string uid = fmt::format("{}.{}.{}", tag_string(tag), name, unique);
    if (!pylabhub::hub::is_valid_identifier(uid, pylabhub::hub::IdentifierKind::RoleUid))
    {
        throw std::invalid_argument(
            fmt::format("make_role_uid: composed uid '{}' fails HEP-CORE-0033 "
                        "§G2.2.0b grammar (tag='{}' name='{}' unique='{}')",
                        uid, tag_string(tag), name, unique));
    }
    return uid;
}

/**
 * @brief Convenience overload — instance number → `inst<NNNNNNNN>`.
 *
 * For tests + tooling that want a stable, sortable unique component
 * from an integer counter.  Format matches the convention used by
 * existing L3 tests (e.g. `prod.m4c_e2e.uid00000001`-style); leading
 * `inst` keeps the human-readable "this is an instance counter"
 * marker so the uid doesn't read as an unstructured number.
 */
[[nodiscard]] inline std::string make_role_uid(RoleUidTag tag, std::string_view name,
                                               std::uint32_t instance)
{
    return make_role_uid(tag, name, fmt::format("inst{:08}", instance));
}

} // namespace pylabhub::scripting
