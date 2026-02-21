#pragma once
/**
 * @file uid_utils.hpp
 * @brief Utilities for generating and validating pylabhub UIDs.
 *
 * ## UID format
 *
 *   Hub:   HUB-{NAME}-{SUFFIX}    (max ~21 chars; fits char[40] SHM field)
 *   Actor: ACTOR-{NAME}-{SUFFIX}  (max ~23 chars; fits char[40] SHM field)
 *
 * Where:
 *   {NAME}   -- Up to 8 uppercase alphanumeric characters derived from the
 *               human-readable name. Non-alphanumeric runs collapse to a single
 *               "-"; leading/trailing "-" are stripped. Falls back to "NODE".
 *   {SUFFIX} -- 8 uppercase hex digits from a 32-bit random value.
 *               Uses std::random_device; falls back to a high-res-clock+Knuth
 *               hash on platforms where entropy() == 0.
 *
 * Examples:
 *   "my lab hub"         -> HUB-MYLABHUB-3A7F2B1C
 *   "Temperature Sensor" -> ACTOR-TEMPERAT-9E1D4C2A
 *   (empty name)         -> HUB-NODE-B3F12E9A
 *
 * Properties:
 *   - Human-readable: name component lets operators identify the source
 *   - Recognisable: same node generates similar names across restarts
 *   - Collision-resistant: 32-bit suffix = 1-in-4 billion chance per pair
 *   - Compact: fits in char[40] SHM fields without truncation
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

/// Derive up to @p max_len uppercase alphanumeric chars from @p name.
/// Non-alnum runs become a single "-"; leading/trailing "-" are stripped.
/// Returns "NODE" when the result would otherwise be empty.
inline std::string sanitize_name_part(const std::string &name, std::size_t max_len = 8)
{
    std::string out;
    out.reserve(max_len + 1);
    for (unsigned char c : name)
    {
        if (out.size() >= max_len)
        {
            break;
        }
        if (std::isalpha(c) != 0)
        {
            out += static_cast<char>(std::toupper(c));
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
    while (!out.empty() && out.back() == '-')
    {
        out.pop_back();
    }
    return out.empty() ? "NODE" : out;
}

/// Returns a 32-bit random value.
/// Prefers std::random_device; falls back to a high-res-clock+Knuth hash on failure.
inline uint32_t random_u32()
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
    const auto ns = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    // Knuth multiplicative hash step (good avalanche)
    return static_cast<uint32_t>((ns ^ (ns >> 17U)) * 2654435761ULL);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public generators
// ---------------------------------------------------------------------------

/**
 * @brief Generate a hub UID: @c "HUB-{NAME}-{8HEX}".
 *
 * @param hub_name Human-readable hub name (e.g. "asu.lab.main").
 *                 May be empty — "NODE" is used in that case.
 * @return         A UID string of the form @c "HUB-MYLABHUB-3A7F2B1C".
 */
inline std::string generate_hub_uid(const std::string &hub_name = "")
{
    const auto name_part = detail::sanitize_name_part(hub_name, 8U);
    char suffix[9];
    std::snprintf(suffix, sizeof(suffix), "%08X", detail::random_u32());
    return "HUB-" + name_part + "-" + suffix;
}

/**
 * @brief Generate an actor UID: @c "ACTOR-{NAME}-{8HEX}".
 *
 * @param actor_name Human-readable actor name (e.g. "TempSensor").
 * @return           A UID string of the form @c "ACTOR-TEMPSENS-9E1D4C2A".
 */
inline std::string generate_actor_uid(const std::string &actor_name = "")
{
    const auto name_part = detail::sanitize_name_part(actor_name, 8U);
    char suffix[9];
    std::snprintf(suffix, sizeof(suffix), "%08X", detail::random_u32());
    return "ACTOR-" + name_part + "-" + suffix;
}

// ---------------------------------------------------------------------------
// Validators
// ---------------------------------------------------------------------------

/// True if @p uid starts with @c "HUB-" and has at least one more character.
inline bool has_hub_prefix(std::string_view uid) noexcept
{
    return uid.size() >= 8U && uid.substr(0, 4) == "HUB-";
}

/// True if @p uid starts with @c "ACTOR-" and has at least one more character.
inline bool has_actor_prefix(std::string_view uid) noexcept
{
    return uid.size() >= 10U && uid.substr(0, 6) == "ACTOR-";
}

} // namespace pylabhub::uid
