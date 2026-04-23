/**
 * @file naming.cpp
 * @brief Identifier validator + parser implementation.
 *
 * Grammar authoritative source: docs/tech_draft/HUB_CHARACTER_PREREQUISITES.md
 * §G2.2.0b "Naming conventions for hub identifiers".  When in doubt,
 * the spec wins — this file tracks it, not the reverse.
 */
#include "utils/naming.hpp"

#include "utils/debug_info.hpp" // PLH_PANIC

#include <algorithm>
#include <cstddef>

namespace pylabhub::hub
{

namespace
{

constexpr std::size_t kMaxComponentLen = 64;
constexpr std::size_t kMaxTotalLen     = 128;

[[nodiscard]] constexpr bool is_first_char(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] constexpr bool is_component_char(char c) noexcept
{
    return is_first_char(c) || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/// Validates one dot-separated NameComponent: [A-Za-z][A-Za-z0-9_-]{0,63}.
[[nodiscard]] bool is_valid_component(std::string_view c) noexcept
{
    if (c.empty() || c.size() > kMaxComponentLen) return false;
    if (!is_first_char(c[0])) return false;
    for (std::size_t i = 1; i < c.size(); ++i)
        if (!is_component_char(c[i])) return false;
    return true;
}

/// Walks the dotted body, invoking @p f on each component.  Returns
/// false if any component is malformed (including empty, which catches
/// leading/trailing dots and `..` runs).
template <typename F>
[[nodiscard]] bool for_each_component(std::string_view body, F &&f)
{
    std::size_t start = 0;
    for (std::size_t i = 0; i <= body.size(); ++i)
    {
        if (i == body.size() || body[i] == '.')
        {
            std::string_view comp = body.substr(start, i - start);
            if (!is_valid_component(comp)) return false;
            if (!f(comp)) return false;
            start = i + 1;
        }
    }
    return true;
}

/// Validates a dot-separated identifier body (no sigil, no prefix).
/// At least one component required; rejects leading/trailing dot and
/// adjacent-dot runs by virtue of `is_valid_component` rejecting
/// empty components.
[[nodiscard]] bool validate_dotted_body(std::string_view body) noexcept
{
    if (body.empty()) return false;
    return for_each_component(body, [](std::string_view) { return true; });
}

[[nodiscard]] std::size_t count_dots(std::string_view s) noexcept
{
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), '.'));
}

constexpr std::string_view kRoleTags[]             = {"prod", "cons", "proc"};
constexpr std::string_view kPeerTag                 = "hub";
constexpr std::string_view kChannelReservedFirst[]  = {"prod", "cons", "proc", "hub", "sys"};

[[nodiscard]] bool is_role_tag(std::string_view s) noexcept
{
    for (auto t : kRoleTags)
        if (s == t) return true;
    return false;
}

[[nodiscard]] bool is_peer_tag(std::string_view s) noexcept
{
    return s == kPeerTag;
}

[[nodiscard]] bool is_reserved_channel_first(std::string_view s) noexcept
{
    for (auto t : kChannelReservedFirst)
        if (s == t) return true;
    return false;
}

/// Shared structural check for tagged uids (role + peer): ≥3 dotted
/// components, each a valid NameComponent.  Tag-set check is the
/// caller's responsibility.
[[nodiscard]] bool is_valid_tagged_uid_structure(std::string_view s) noexcept
{
    if (!validate_dotted_body(s)) return false;
    return count_dots(s) >= 2;
}

/// Returns the substring before the first dot, or the full string if
/// no dot is present.
[[nodiscard]] std::string_view first_component(std::string_view s) noexcept
{
    auto d = s.find('.');
    return (d == std::string_view::npos) ? s : s.substr(0, d);
}

} // namespace

const char *to_string(IdentifierKind k) noexcept
{
    switch (k)
    {
    case IdentifierKind::Channel:  return "Channel";
    case IdentifierKind::Band:     return "Band";
    case IdentifierKind::RoleUid:  return "RoleUid";
    case IdentifierKind::RoleName: return "RoleName";
    case IdentifierKind::PeerUid:  return "PeerUid";
    case IdentifierKind::Schema:   return "Schema";
    case IdentifierKind::SysKey:   return "SysKey";
    }
    return "Unknown";
}

bool is_valid_identifier(std::string_view s, IdentifierKind kind) noexcept
{
    if (s.empty() || s.size() > kMaxTotalLen) return false;

    switch (kind)
    {
    case IdentifierKind::Channel:
    {
        if (!validate_dotted_body(s)) return false;
        return !is_reserved_channel_first(first_component(s));
    }

    case IdentifierKind::RoleName:
        return validate_dotted_body(s);

    case IdentifierKind::Band:
        return s.front() == '!' && validate_dotted_body(s.substr(1));

    case IdentifierKind::Schema:
        return s.front() == '$' && validate_dotted_body(s.substr(1));

    case IdentifierKind::RoleUid:
        return is_valid_tagged_uid_structure(s) &&
               is_role_tag(first_component(s));

    case IdentifierKind::PeerUid:
        return is_valid_tagged_uid_structure(s) &&
               is_peer_tag(first_component(s));

    case IdentifierKind::SysKey:
    {
        // 'sys' + '.' + ≥1 component
        if (s.size() < 5) return false;
        if (s.substr(0, 4) != "sys.") return false;
        return validate_dotted_body(s);
    }
    }
    return false;
}

void require_valid_identifier(std::string_view s, IdentifierKind kind,
                              std::string_view context)
{
    if (!is_valid_identifier(s, kind))
    {
        PLH_PANIC("invalid identifier: '{}' (kind={}) ctx='{}'",
                  s, to_string(kind), context);
    }
}

namespace
{
/// Internal helper — splits a validated tagged-uid into its three
/// mandatory segments without repeating the validation.
TaggedUidParts split_tagged_uid(std::string_view uid) noexcept
{
    const auto     d1 = uid.find('.');
    const auto     d2 = uid.find('.', d1 + 1);
    TaggedUidParts parts;
    parts.tag    = uid.substr(0, d1);
    parts.name   = uid.substr(d1 + 1, d2 - d1 - 1);
    parts.unique = uid.substr(d2 + 1);
    return parts;
}
} // namespace

std::optional<TaggedUidParts> parse_role_uid(std::string_view uid) noexcept
{
    if (!is_valid_identifier(uid, IdentifierKind::RoleUid)) return std::nullopt;
    return split_tagged_uid(uid);
}

std::optional<TaggedUidParts> parse_peer_uid(std::string_view uid) noexcept
{
    if (!is_valid_identifier(uid, IdentifierKind::PeerUid)) return std::nullopt;
    return split_tagged_uid(uid);
}

std::optional<std::string_view> extract_role_tag(std::string_view uid) noexcept
{
    if (auto p = parse_role_uid(uid)) return p->tag;
    return std::nullopt;
}

std::string format_role_ref(std::string_view uid,
                            std::string_view name,
                            std::string_view tag)
{
    if (!uid.empty()) return std::string(uid);

    std::string out;
    out.reserve(tag.size() + name.size() + 4);
    if (!tag.empty())
    {
        out.push_back('[');
        out.append(tag.data(), tag.size());
        out.push_back(']');
        if (!name.empty()) out.push_back(' ');
    }
    out.append(name.data(), name.size());
    return out;
}

} // namespace pylabhub::hub
