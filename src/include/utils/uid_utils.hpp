#pragma once
/**
 * @file uid_utils.hpp
 * @brief Utilities for generating and validating pylabhub UIDs.
 *
 * ## UID format
 *
 *   Hub:       HUB-{NAME}-{SUFFIX}   (max ~21 chars; fits char[40] SHM field)
 *   Producer:  PROD-{NAME}-{SUFFIX}  (max ~22 chars)
 *   Consumer:  CONS-{NAME}-{SUFFIX}  (max ~22 chars)
 *   Processor: PROC-{NAME}-{SUFFIX}  (max ~22 chars)
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
 *   "Temperature Sensor" -> PROD-TEMPERAT-9E1D4C2A
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
 * @brief Generate a UID: @c "{PREFIX}-{NAME}-{8HEX}".
 *
 * @param prefix Role prefix (e.g. "HUB", "PROD", "CONS", "PROC").
 * @param name   Human-readable name (e.g. "TempSensor").
 *               May be empty — "NODE" is used in that case.
 * @return       A UID string of the form @c "PROD-TEMPSENS-3A7F2B1C".
 */
inline std::string generate_uid(std::string_view prefix, const std::string &name = "")
{
    const auto name_part = detail::sanitize_name_part(name, 8U);
    char suffix[9];
    std::snprintf(suffix, sizeof(suffix), "%08X", detail::random_u32());
    return std::string(prefix) + "-" + name_part + "-" + suffix;
}

inline std::string generate_hub_uid(const std::string &name = "")       { return generate_uid("HUB", name); }
inline std::string generate_producer_uid(const std::string &name = "")  { return generate_uid("PROD", name); }
inline std::string generate_consumer_uid(const std::string &name = "")  { return generate_uid("CONS", name); }
inline std::string generate_processor_uid(const std::string &name = "") { return generate_uid("PROC", name); }

// ---------------------------------------------------------------------------
// Validators
// ---------------------------------------------------------------------------

/// True if @p uid starts with @c "{prefix}-" and has content after it.
inline bool has_uid_prefix(std::string_view uid, std::string_view prefix) noexcept
{
    return uid.size() > prefix.size() + 1 &&
           uid.substr(0, prefix.size()) == prefix &&
           uid[prefix.size()] == '-';
}

inline bool has_hub_prefix(std::string_view uid) noexcept       { return has_uid_prefix(uid, "HUB"); }
inline bool has_producer_prefix(std::string_view uid) noexcept  { return has_uid_prefix(uid, "PROD"); }
inline bool has_consumer_prefix(std::string_view uid) noexcept  { return has_uid_prefix(uid, "CONS"); }
inline bool has_processor_prefix(std::string_view uid) noexcept { return has_uid_prefix(uid, "PROC"); }

} // namespace pylabhub::uid
