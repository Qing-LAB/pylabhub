/**
 * @file test_uuid_and_format.cpp
 * @brief Unit tests for uuid_utils (generate_uuid4, is_valid_uuid4) and
 *        format_tools (bytes_to_hex, bytes_from_hex).
 *
 * TG-01 + TG-02 from REVIEW_FullStack_2026-03-17.
 */

#include "utils/uuid_utils.hpp"
#include "utils/format_tools.hpp"
#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace pylabhub::utils;
using pylabhub::format_tools::bytes_from_hex;
using pylabhub::format_tools::bytes_to_hex;

// ============================================================================
// uuid_utils — generate_uuid4
// ============================================================================

TEST(UuidUtilsTest, GenerateUuid4_IsValidFormat)
{
    const auto uuid = generate_uuid4();
    EXPECT_EQ(uuid.size(), 36u);
    EXPECT_TRUE(is_valid_uuid4(uuid)) << "Generated UUID is invalid: " << uuid;
}

TEST(UuidUtilsTest, GenerateUuid4_HasVersion4Nibble)
{
    const auto uuid = generate_uuid4();
    EXPECT_EQ(uuid[14], '4') << "Version nibble must be 4: " << uuid;
}

TEST(UuidUtilsTest, GenerateUuid4_HasCorrectVariant)
{
    const auto uuid = generate_uuid4();
    const char v = uuid[19];
    EXPECT_TRUE(v == '8' || v == '9' || v == 'a' || v == 'b')
        << "Variant nibble must be 8/9/a/b, got '" << v << "': " << uuid;
}

TEST(UuidUtilsTest, GenerateUuid4_DashPositions)
{
    const auto uuid = generate_uuid4();
    EXPECT_EQ(uuid[8], '-');
    EXPECT_EQ(uuid[13], '-');
    EXPECT_EQ(uuid[18], '-');
    EXPECT_EQ(uuid[23], '-');
}

TEST(UuidUtilsTest, GenerateUuid4_LowercaseHex)
{
    const auto uuid = generate_uuid4();
    for (size_t i = 0; i < uuid.size(); ++i)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            continue;
        EXPECT_TRUE((uuid[i] >= '0' && uuid[i] <= '9') ||
                     (uuid[i] >= 'a' && uuid[i] <= 'f'))
            << "Non-lowercase hex at position " << i << ": " << uuid;
    }
}

TEST(UuidUtilsTest, GenerateUuid4_Uniqueness)
{
    // Generate 100 UUIDs and verify no duplicates.
    std::set<std::string> uuids;
    for (int i = 0; i < 100; ++i)
    {
        const auto uuid = generate_uuid4();
        EXPECT_TRUE(uuids.insert(uuid).second)
            << "Duplicate UUID at iteration " << i << ": " << uuid;
    }
    EXPECT_EQ(uuids.size(), 100u);
}

// ============================================================================
// uuid_utils — is_valid_uuid4
// ============================================================================

TEST(UuidUtilsTest, IsValid_RejectsEmpty)
{
    EXPECT_FALSE(is_valid_uuid4(""));
}

TEST(UuidUtilsTest, IsValid_RejectsTooShort)
{
    EXPECT_FALSE(is_valid_uuid4("e7a9f3b2-4c11-4a87-9f1d"));
}

TEST(UuidUtilsTest, IsValid_RejectsTooLong)
{
    EXPECT_FALSE(is_valid_uuid4("e7a9f3b2-4c11-4a87-9f1d-c2d04b3e1a56X"));
}

TEST(UuidUtilsTest, IsValid_RejectsWrongVersion)
{
    // Version nibble at position 14 must be '4', here we use '3'
    EXPECT_FALSE(is_valid_uuid4("e7a9f3b2-4c11-3a87-9f1d-c2d04b3e1a56"));
}

TEST(UuidUtilsTest, IsValid_RejectsWrongVariant)
{
    // Variant nibble at position 19 must be 8/9/a/b, here we use 'c'
    EXPECT_FALSE(is_valid_uuid4("e7a9f3b2-4c11-4a87-cf1d-c2d04b3e1a56"));
}

TEST(UuidUtilsTest, IsValid_RejectsMissingDash)
{
    // Remove dash at position 8
    EXPECT_FALSE(is_valid_uuid4("e7a9f3b2X4c11-4a87-9f1d-c2d04b3e1a56"));
}

TEST(UuidUtilsTest, IsValid_RejectsNonHexChar)
{
    // 'g' is not a hex digit
    EXPECT_FALSE(is_valid_uuid4("g7a9f3b2-4c11-4a87-9f1d-c2d04b3e1a56"));
}

TEST(UuidUtilsTest, IsValid_AcceptsUppercaseVariant)
{
    // Uppercase 'A' and 'B' at variant position should be valid
    EXPECT_TRUE(is_valid_uuid4("e7a9f3b2-4c11-4a87-Af1d-c2d04b3e1a56"));
    EXPECT_TRUE(is_valid_uuid4("e7a9f3b2-4c11-4a87-Bf1d-c2d04b3e1a56"));
}

TEST(UuidUtilsTest, IsValid_AcceptsKnownGood)
{
    EXPECT_TRUE(is_valid_uuid4("e7a9f3b2-4c11-4a87-9f1d-c2d04b3e1a56"));
    EXPECT_TRUE(is_valid_uuid4("00000000-0000-4000-8000-000000000000"));
}

// ============================================================================
// format_tools — bytes_to_hex
// ============================================================================

TEST(FormatToolsHexTest, BytesToHex_Empty)
{
    EXPECT_EQ(bytes_to_hex(""), "");
}

TEST(FormatToolsHexTest, BytesToHex_SingleByte)
{
    // Byte 0xFF -> "ff"
    EXPECT_EQ(bytes_to_hex(std::string_view("\xff", 1)), "ff");
    // Byte 0x00 -> "00"
    EXPECT_EQ(bytes_to_hex(std::string_view("\x00", 1)), "00");
    // Byte 0x0A -> "0a"
    EXPECT_EQ(bytes_to_hex(std::string_view("\x0a", 1)), "0a");
}

TEST(FormatToolsHexTest, BytesToHex_MultipleBytes)
{
    const std::string raw = {'\x48', '\x65', '\x6c', '\x6c', '\x6f'}; // "Hello"
    EXPECT_EQ(bytes_to_hex(raw), "48656c6c6f");
}

TEST(FormatToolsHexTest, BytesToHex_AllZeros)
{
    const std::string raw(4, '\x00');
    EXPECT_EQ(bytes_to_hex(raw), "00000000");
}

TEST(FormatToolsHexTest, BytesToHex_IsLowercase)
{
    // 0xAB should produce "ab" not "AB"
    EXPECT_EQ(bytes_to_hex(std::string_view("\xab", 1)), "ab");
}

// ============================================================================
// format_tools — bytes_from_hex
// ============================================================================

TEST(FormatToolsHexTest, BytesFromHex_Empty)
{
    EXPECT_EQ(bytes_from_hex(""), "");
}

TEST(FormatToolsHexTest, BytesFromHex_ValidLowercase)
{
    const auto result = bytes_from_hex("48656c6c6f");
    EXPECT_EQ(result, "Hello");
}

TEST(FormatToolsHexTest, BytesFromHex_ValidUppercase)
{
    const auto result = bytes_from_hex("48656C6C6F");
    EXPECT_EQ(result, "Hello");
}

TEST(FormatToolsHexTest, BytesFromHex_MixedCase)
{
    const auto result = bytes_from_hex("48656c6C6f");
    EXPECT_EQ(result, "Hello");
}

TEST(FormatToolsHexTest, BytesFromHex_OddLength_ReturnsInput)
{
    // Odd length is invalid — returns input unchanged
    const auto result = bytes_from_hex("abc");
    EXPECT_EQ(result, "abc");
}

TEST(FormatToolsHexTest, BytesFromHex_InvalidChar_ReturnsInput)
{
    // 'g' is not a hex char — returns input unchanged
    const auto result = bytes_from_hex("gg");
    EXPECT_EQ(result, "gg");
}

TEST(FormatToolsHexTest, BytesFromHex_InvalidCharMiddle_ReturnsInput)
{
    const auto result = bytes_from_hex("48zz6c");
    EXPECT_EQ(result, "48zz6c");
}

// ============================================================================
// format_tools — roundtrip
// ============================================================================

TEST(FormatToolsHexTest, Roundtrip_BinaryData)
{
    // Construct binary data including null bytes
    const std::string original = {'\x00', '\x01', '\x7f', '\x80', '\xfe', '\xff'};
    const auto hex = bytes_to_hex(original);
    const auto recovered = bytes_from_hex(hex);
    EXPECT_EQ(recovered, original);
    EXPECT_EQ(hex, "00017f80feff");
}

TEST(FormatToolsHexTest, Roundtrip_EmptyString)
{
    EXPECT_EQ(bytes_from_hex(bytes_to_hex("")), "");
}
