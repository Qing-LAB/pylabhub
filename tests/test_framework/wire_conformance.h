#pragma once
/**
 * @file wire_conformance.h
 * @brief Reusable wire-conformance assertions for protocol regression tests.
 *
 * Audit TR1 (2026-05-17) — generalizes the pattern introduced by the
 * B1+B2 regression test.  The audit found that **no test in the
 * pre-2026-05-17 suite pinned a wire payload key set against a HEP
 * spec** — BRC and broker agreed on the wrong key (`channel` vs
 * `band`), round-trip tests passed, and a class of "rename
 * incomplete" bugs slipped through.  These helpers make it easy to
 * write the kind of test that would have caught B1: assert exact
 * presence / absence of fields against an authoritative HEP §.
 *
 * The helpers are header-only — no library dependency.  Use from any
 * L2/L3 test worker that wants to lock down a wire-message shape.
 *
 * Usage:
 *   nlohmann::json ack = bc.register_channel(opts, 3000).value();
 *   expect_object_has_keys(ack, {"status", "channel_id", "heartbeat"});
 *   expect_object_lacks_keys(ack, {"channel", "band"});  // legacy/wrong
 *   expect_object_has_keys(ack["heartbeat"],
 *       {"heartbeat_interval_ms", "ready_miss_heartbeats",
 *        "pending_miss_heartbeats"});
 *
 * Mutation discipline: every helper emits a precise diagnostic that
 * names the missing/unexpected key + the HEP § it violates, so a
 * failing test points the next reader at the right doc § without
 * needing to dig.
 */
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <initializer_list>
#include <string>
#include <string_view>

namespace pylabhub::tests::wire
{

/// Assert the JSON object @p body contains every key in @p required.
/// Test fails with one EXPECT per missing key, naming the key + the
/// HEP § (@p hep_section) the requirement comes from.  @p msg_name
/// is the wire message family ("REG_ACK", "BAND_JOIN_ACK", etc.).
inline void expect_object_has_keys(const nlohmann::json &body,
                                   std::initializer_list<std::string_view> required,
                                   std::string_view msg_name, std::string_view hep_section)
{
    ASSERT_TRUE(body.is_object()) << "wire conformance: " << msg_name
                                  << " body must be a JSON object (per " << hep_section << ")";
    for (const auto &k : required)
    {
        EXPECT_TRUE(body.contains(std::string(k)))
            << "wire conformance: " << msg_name << " missing required key `" << k << "` (per "
            << hep_section
            << ").  Either the broker is not "
               "emitting it, or the field was renamed without "
               "updating the spec.";
    }
}

/// Assert the JSON object @p body does NOT contain any of @p forbidden.
/// Catches stale keys that should have been renamed (e.g., `channel`
/// on a BAND_*_NOTIFY body — the legacy name that B1 fixed).
inline void expect_object_lacks_keys(const nlohmann::json &body,
                                     std::initializer_list<std::string_view> forbidden,
                                     std::string_view msg_name, std::string_view hep_section)
{
    if (!body.is_object())
        return; // expect_object_has_keys reports
    for (const auto &k : forbidden)
    {
        EXPECT_FALSE(body.contains(std::string(k)))
            << "wire conformance: " << msg_name << " has forbidden key `" << k << "` (per "
            << hep_section
            << ").  This is typically a stale name "
               "from before a rename refactor — the spec authoritative.";
    }
}

/// Assert @p body[@p key] is a string with exact value @p expected.
/// Combines presence + type + content into one expectation.
inline void expect_string_field(const nlohmann::json &body, std::string_view key,
                                std::string_view expected, std::string_view msg_name,
                                std::string_view hep_section)
{
    ASSERT_TRUE(body.is_object()) << "wire conformance: " << msg_name
                                  << " body must be a JSON object (per " << hep_section << ")";
    const std::string k(key);
    ASSERT_TRUE(body.contains(k)) << "wire conformance: " << msg_name << " missing field `" << key
                                  << "` (per " << hep_section << ")";
    ASSERT_TRUE(body.at(k).is_string()) << "wire conformance: " << msg_name << " field `" << key
                                        << "` must be a string (per " << hep_section << ")";
    EXPECT_EQ(body.at(k).get<std::string>(), std::string(expected))
        << "wire conformance: " << msg_name << " field `" << key << "` content mismatch (per "
        << hep_section << ")";
}

/// Assert @p body[@p key] is an integer.  Used for typed checks on
/// heartbeat block fields, member counts, etc.
inline void expect_int_field(const nlohmann::json &body, std::string_view key,
                             std::string_view msg_name, std::string_view hep_section)
{
    ASSERT_TRUE(body.is_object()) << "wire conformance: " << msg_name
                                  << " body must be a JSON object (per " << hep_section << ")";
    const std::string k(key);
    ASSERT_TRUE(body.contains(k)) << "wire conformance: " << msg_name << " missing field `" << key
                                  << "` (per " << hep_section << ")";
    EXPECT_TRUE(body.at(k).is_number_integer())
        << "wire conformance: " << msg_name << " field `" << key << "` must be an integer (per "
        << hep_section << ")";
}

} // namespace pylabhub::tests::wire
