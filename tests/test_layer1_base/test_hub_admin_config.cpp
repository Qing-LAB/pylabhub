/**
 * @file test_hub_admin_config.cpp
 * @brief L1 pure-function tests for parse_hub_admin_config — the
 *        `admin.output_buffer` console-buffer caps (HEP-CORE-0033 §11.0.4).
 *
 * Pins the parse + strict validation: defaults when absent, custom caps
 * parsed, and the fail-fast rejections (unknown key, zero cap, per-line cap
 * above the total cap).
 */

#include "utils/config/hub_admin_config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using nlohmann::json;
using pylabhub::config::parse_hub_admin_config;

namespace
{
/// Wrap an `admin.output_buffer` object into the full config shape the parser
/// reads.
json with_output_buffer(json ob)
{
    return json{{"admin", {{"output_buffer", std::move(ob)}}}};
}
}  // namespace

TEST(HubAdminConfigParse, DefaultsWhenAbsent)
{
    const auto ac = parse_hub_admin_config(json::object());
    EXPECT_TRUE(ac.enabled);
    EXPECT_EQ(ac.output_buffer.max_lines, 1000U);
    EXPECT_EQ(ac.output_buffer.max_bytes, 1024U * 1024U);
    EXPECT_EQ(ac.output_buffer.max_line_bytes, 64U * 1024U);
}

TEST(HubAdminConfigParse, CustomCapsParsed)
{
    const auto ac = parse_hub_admin_config(
        with_output_buffer({{"max_lines", 50}, {"max_bytes", 4096}, {"max_line_bytes", 512}}));
    EXPECT_EQ(ac.output_buffer.max_lines, 50U);
    EXPECT_EQ(ac.output_buffer.max_bytes, 4096U);
    EXPECT_EQ(ac.output_buffer.max_line_bytes, 512U);
}

TEST(HubAdminConfigParse, PartialCapsKeepDefaults)
{
    // Only max_lines given → the other two keep their defaults.
    const auto ac = parse_hub_admin_config(with_output_buffer({{"max_lines", 7}}));
    EXPECT_EQ(ac.output_buffer.max_lines, 7U);
    EXPECT_EQ(ac.output_buffer.max_bytes, 1024U * 1024U);
    EXPECT_EQ(ac.output_buffer.max_line_bytes, 64U * 1024U);
}

TEST(HubAdminConfigParse, UnknownKeyThrows)
{
    EXPECT_THROW(parse_hub_admin_config(with_output_buffer({{"bogus", 1}})), std::runtime_error);
}

TEST(HubAdminConfigParse, NotAnObjectThrows)
{
    EXPECT_THROW(parse_hub_admin_config(json{{"admin", {{"output_buffer", 5}}}}),
                 std::runtime_error);
}

TEST(HubAdminConfigParse, ZeroCapThrows)
{
    EXPECT_THROW(parse_hub_admin_config(with_output_buffer({{"max_lines", 0}})), std::runtime_error);
    EXPECT_THROW(parse_hub_admin_config(with_output_buffer({{"max_bytes", 0}})), std::runtime_error);
    EXPECT_THROW(parse_hub_admin_config(with_output_buffer({{"max_line_bytes", 0}})),
                 std::runtime_error);
}

TEST(HubAdminConfigParse, LineCapAboveTotalThrows)
{
    EXPECT_THROW(
        parse_hub_admin_config(with_output_buffer({{"max_bytes", 100}, {"max_line_bytes", 200}})),
        std::runtime_error);
}
