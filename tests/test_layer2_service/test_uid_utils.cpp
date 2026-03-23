/**
 * @file test_uid_utils.cpp
 * @brief Unit tests for uid_utils.hpp: generators, validators, sanitize_name_part.
 *
 * Pure API test — no lifecycle, no workers, no external dependencies.
 * All functions are header-only inline.
 */
#include "utils/uid_utils.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace pylabhub::uid;

class UidUtilsTest : public ::testing::Test
{
};

// ============================================================================
// Generators — prefix and basic structure
// ============================================================================

TEST_F(UidUtilsTest, GenerateHubUid_HasPrefix)
{
    auto uid = generate_hub_uid();
    EXPECT_TRUE(uid.starts_with("HUB-")) << "uid=" << uid;
    EXPECT_GT(uid.size(), 4u);
}

TEST_F(UidUtilsTest, GenerateUid_CustomPrefix)
{
    auto uid = generate_uid("CUSTOM", "TestNode");
    EXPECT_TRUE(uid.starts_with("CUSTOM-")) << "uid=" << uid;
    EXPECT_NE(uid.find("TESTNOD"), std::string::npos) << "uid=" << uid;
}

TEST_F(UidUtilsTest, GenerateProcessorUid_HasPrefix)
{
    auto uid = generate_processor_uid();
    EXPECT_TRUE(uid.starts_with("PROC-")) << "uid=" << uid;
    EXPECT_GT(uid.size(), 5u);
}

TEST_F(UidUtilsTest, GenerateProducerUid_HasPrefix)
{
    auto uid = generate_producer_uid();
    EXPECT_TRUE(uid.starts_with("PROD-")) << "uid=" << uid;
    EXPECT_GT(uid.size(), 5u);
}

TEST_F(UidUtilsTest, GenerateConsumerUid_HasPrefix)
{
    auto uid = generate_consumer_uid();
    EXPECT_TRUE(uid.starts_with("CONS-")) << "uid=" << uid;
    EXPECT_GT(uid.size(), 5u);
}

// ============================================================================
// Generators — with name
// ============================================================================

TEST_F(UidUtilsTest, GenerateHubUid_WithName)
{
    auto uid = generate_hub_uid("myHub");
    EXPECT_TRUE(uid.starts_with("HUB-")) << "uid=" << uid;
    // Sanitized name "myHub" → "MYHUB" should appear in the UID
    EXPECT_NE(uid.find("MYHUB"), std::string::npos) << "uid=" << uid;
}

TEST_F(UidUtilsTest, GenerateProducerUid_WithName)
{
    auto uid = generate_producer_uid("TempSensor");
    EXPECT_TRUE(uid.starts_with("PROD-")) << "uid=" << uid;
    // "TempSensor" → "TEMPSEN" or "TEMPSENS" (up to 8 chars)
    EXPECT_NE(uid.find("TEMP"), std::string::npos) << "uid=" << uid;
}

TEST_F(UidUtilsTest, GenerateUid_Uniqueness)
{
    // 100 calls must all produce unique UIDs
    std::set<std::string> uids;
    for (int i = 0; i < 100; ++i)
    {
        uids.insert(generate_hub_uid("test"));
    }
    EXPECT_EQ(uids.size(), 100u) << "Expected 100 unique UIDs";
}

// ============================================================================
// Validators
// ============================================================================

TEST_F(UidUtilsTest, HasHubPrefix_Valid)
{
    EXPECT_TRUE(has_hub_prefix("HUB-FOO-ABCD1234"));
}

TEST_F(UidUtilsTest, HasHubPrefix_Invalid)
{
    EXPECT_FALSE(has_hub_prefix("PROD-FOO-1234ABCD"));
    EXPECT_FALSE(has_hub_prefix("HUB-"));   // too short (size < 8)
    EXPECT_FALSE(has_hub_prefix(""));
}

TEST_F(UidUtilsTest, HasProcessorPrefix_Valid)
{
    EXPECT_TRUE(has_processor_prefix("PROC-NODE-12345678"));
}

TEST_F(UidUtilsTest, HasProducerPrefix_Valid)
{
    EXPECT_TRUE(has_producer_prefix("PROD-TEMP-AABBCCDD"));
}

TEST_F(UidUtilsTest, HasConsumerPrefix_Valid)
{
    EXPECT_TRUE(has_consumer_prefix("CONS-LOG-11223344"));
}

// ============================================================================
// sanitize_name_part (detail namespace — testing via public generators)
// ============================================================================

TEST_F(UidUtilsTest, Sanitize_Normal)
{
    auto result = detail::sanitize_name_part("myNode");
    EXPECT_EQ(result, "MYNODE");
}

TEST_F(UidUtilsTest, Sanitize_SpecialChars)
{
    // Non-alnum chars collapse to a single dash
    auto result = detail::sanitize_name_part("my.node!v2");
    // "my" → "MY", "." → "-", "node" → "NODE", "!" → collapsed, "v2" → "V2"
    // Expected: "MY-NODE-V" (truncated to 8 chars)
    EXPECT_LE(result.size(), 8u);
    EXPECT_TRUE(result.starts_with("MY")) << "result=" << result;
    EXPECT_NE(result.find("NODE"), std::string::npos) << "result=" << result;
}

TEST_F(UidUtilsTest, Sanitize_Empty)
{
    auto result = detail::sanitize_name_part("");
    EXPECT_EQ(result, "NODE");
}

TEST_F(UidUtilsTest, Sanitize_TooLong)
{
    // 20-char input should be truncated to max_len (default 8)
    auto result = detail::sanitize_name_part("abcdefghijklmnopqrst");
    EXPECT_LE(result.size(), 8u);
    EXPECT_EQ(result, "ABCDEFGH");
}

TEST_F(UidUtilsTest, Sanitize_LeadingTrailingDash)
{
    // Leading/trailing dashes stripped; internal non-alnum → single dash
    auto result = detail::sanitize_name_part("---foo---");
    EXPECT_EQ(result, "FOO");
}
