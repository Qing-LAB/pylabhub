#pragma once
/**
 * @file uid_utils.hpp
 * @brief Utilities for generating pylabhub role + hub UIDs.
 *
 * ## UID format (HEP-0033 §G2.2.0b)
 *
 *   Role:  <role_tag>.<name>.uid<8hex>
 *          <role_tag> ∈ {prod, cons, proc}
 *   Peer:  hub.<name>.uid<8hex>
 *
 * Examples:
 *   "my lab hub"         -> hub.mylabhub.uid3a7f2b1c
 *   "Temperature Sensor" -> prod.temperaturesensor.uid9e1d4c2a   (truncated to 32)
 *   (empty name)         -> hub.node.uidb3f12e9a
 *
 * Where:
 *   <name>   — Up to 32 chars, lowercase letters + digits + dash. Derived
 *              from the human-readable name by lowercasing letters, keeping
 *              digits, collapsing runs of non-alnum to a single `-`, and
 *              stripping leading/trailing `-`. If the first char of the
 *              sanitized result is a digit, 'n' is prepended so the
 *              NameComponent `[A-Za-z]` first-char rule holds. Falls back
 *              to "node" when the result would otherwise be empty.
 *   uid<8hex> — `uid` full-word prefix + 8 lowercase hex digits from a
 *              32-bit random value.  The `uid` prefix is the project-wide
 *              convention for "opaque random unique suffix" (HEP-0033
 *              §G2.2.0b "Numeric-token prefix convention") and satisfies
 *              the grammar's `[A-Za-z]` first-char rule.  Uses
 *              std::random_device; falls back to a high-res clock +
 *              Knuth hash on platforms where entropy() == 0.
 *
 * Properties
 *   - Human-readable: the `<name>` component lets operators identify source
 *     at a glance.
 *   - Self-classifying: every uid starts with a reserved role tag, so its
 *     kind is derivable from the string alone (see naming.hpp).
 *   - Collision-resistant: 32-bit suffix = ~1-in-4-billion chance per pair.
 *   - Compact: fits comfortably within the 128-char total + 64-per-component
 *     caps in the HEP-0033 grammar.
 *
 * ## Migration note
 *
 * This file previously emitted `PREFIX-NAME-8HEX` uppercase-dashed uids
 * (e.g. `PROD-TEMPSENS-3A7F2B1C`). That format FAILS the HEP-0033
 * §G2.2.0b grammar and is rejected by `identity_config.hpp` at parse
 * time. Any config file with an old-format uid must either clear the
 * `uid` field (so auto-gen runs) or be updated to the new form.
 */

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>

namespace pylabhub::uid
{

namespace detail
{

constexpr std::size_t kMaxNamePart = 32;

/// Derive up to @p max_len lowercase-alphanumeric-dash chars from @p name.
/// Non-alnum runs collapse to a single `-`; leading/trailing `-` are
/// stripped. If the first char of the sanitized result is a digit, 'n'
/// is prepended so the result is a valid `NameComponent`. Falls back
/// to "node" if sanitization leaves nothing usable.
inline std::string sanitize_name_part(const std::string &name,
                                      std::size_t        max_len = kMaxNamePart)
{
    std::string out;
    out.reserve(max_len + 1);
    for (unsigned char c : name)
    {
        if (out.size() >= max_len) break;
        if (std::isalpha(c) != 0)
        {
            out += static_cast<char>(std::tolower(c));
        }
        else if (std::isdigit(c) != 0)
        {
            out += static_cast<char>(c);
        }
        else if (!out.empty() && out.back() != '-')
        {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) return "node";

    // Grammar requires NameComponent to start with [A-Za-z]. If the
    // sanitized result begins with a digit (e.g. name="123Temp" →
    // "123temp"), prepend 'n' to make it legal.
    if (std::isdigit(static_cast<unsigned char>(out.front())) != 0)
    {
        if (out.size() + 1 > max_len) out.pop_back();
        out.insert(out.begin(), 'n');
    }
    return out;
}

/// Returns a 32-bit random value.
/// Prefers std::random_device; falls back to a high-res-clock+Knuth hash on failure.
inline std::uint32_t random_u32()
{
    try
    {
        std::random_device rd;
        if (rd.entropy() > 0.0)
        {
            return rd();
        }
    }
    catch (...) {}
    // Fallback: mix high-res timestamp (usually unique per invocation).
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    // Knuth multiplicative hash step (good avalanche).
    return static_cast<std::uint32_t>((ns ^ (ns >> 17U)) * 2654435761ULL);
}

/// Format a 32-bit value as `uid<8 lowercase hex>`. Example: `uid3a7f2b1c`.
/// The `uid` prefix is the project-wide convention for "opaque random
/// unique suffix" (HEP-0033 §G2.2.0b "Numeric-token prefix convention")
/// and satisfies the `NameComponent [A-Za-z]{first}` grammar rule.
inline std::string unique_suffix()
{
    char buf[12]; // "uid" + 8 hex + NUL
    std::snprintf(buf, sizeof(buf), "uid%08x", random_u32());
    return std::string(buf);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public generators (HEP-0033 §G2.2.0b)
// ---------------------------------------------------------------------------

/**
 * @brief Generate a tagged UID: @c "<tag>.<name>.uid<8hex>".
 *
 * @param tag   Role / peer tag. One of `"hub"`, `"prod"`, `"cons"`, `"proc"`.
 *              Pass it lowercase — the output uid is all lowercase.
 * @param name  Human-readable name. May be empty — "node" is used.
 *
 * The returned string is guaranteed to validate as the matching
 * `IdentifierKind::RoleUid` (for prod/cons/proc) or
 * `IdentifierKind::PeerUid` (for hub) under `naming.hpp`.
 */
inline std::string generate_uid(std::string_view tag, const std::string &name = "")
{
    return std::string(tag) + "." + detail::sanitize_name_part(name) + "."
         + detail::unique_suffix();
}

inline std::string generate_hub_uid(const std::string &name = "")       { return generate_uid("hub",  name); }
inline std::string generate_producer_uid(const std::string &name = "")  { return generate_uid("prod", name); }
inline std::string generate_consumer_uid(const std::string &name = "")  { return generate_uid("cons", name); }
inline std::string generate_processor_uid(const std::string &name = "") { return generate_uid("proc", name); }

// ---------------------------------------------------------------------------
// Prefix checks
// ---------------------------------------------------------------------------
//
// Light-weight `starts-with-<tag>.` checks. For full grammar validation
// (length caps, component-shape, reserved-word conflicts), use
// `naming::is_valid_identifier(uid, IdentifierKind::RoleUid/PeerUid)`
// from `utils/naming.hpp` instead. These helpers are kept for call sites
// that only need a quick kind-discriminator on a uid already known to be
// well-formed.

inline bool has_tag_prefix(std::string_view uid, std::string_view tag) noexcept
{
    return uid.size() > tag.size() + 1 &&
           uid.substr(0, tag.size()) == tag &&
           uid[tag.size()] == '.';
}

inline bool has_hub_prefix(std::string_view uid) noexcept       { return has_tag_prefix(uid, "hub"); }
inline bool has_producer_prefix(std::string_view uid) noexcept  { return has_tag_prefix(uid, "prod"); }
inline bool has_consumer_prefix(std::string_view uid) noexcept  { return has_tag_prefix(uid, "cons"); }
inline bool has_processor_prefix(std::string_view uid) noexcept { return has_tag_prefix(uid, "proc"); }

/// Deprecated spelling retained so existing call sites compile. Prefer
/// `has_tag_prefix`; both return identical results.
inline bool has_uid_prefix(std::string_view uid, std::string_view tag) noexcept
{
    return has_tag_prefix(uid, tag);
}

} // namespace pylabhub::uid
