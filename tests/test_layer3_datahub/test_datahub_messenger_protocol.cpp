/**
 * @file test_datahub_messenger_protocol.cpp
 * @brief Unit tests for hex_encode_schema_hash / hex_decode_schema_hash.
 *
 * These are pure inline functions in messenger_internal.hpp with zero
 * prior test coverage.
 */

#include "messenger_internal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>

using pylabhub::hub::internal::hex_encode_schema_hash;
using pylabhub::hub::internal::hex_decode_schema_hash;
using pylabhub::hub::internal::kSchemaHashBytes;
using pylabhub::hub::internal::kSchemaHashHexLen;

// ── Roundtrip: encode → decode ──────────────────────────────────────────────

TEST(MessengerHexCodecTest, RoundtripArbitraryBytes)
{
    // Construct a 32-byte raw string with varied byte values.
    std::string raw(kSchemaHashBytes, '\0');
    for (size_t i = 0; i < kSchemaHashBytes; ++i)
        raw[i] = static_cast<char>(i * 7 + 3); // arbitrary deterministic pattern

    const std::string hex     = hex_encode_schema_hash(raw);
    const std::string decoded = hex_decode_schema_hash(hex);
    EXPECT_EQ(decoded, raw);
}

// ── Empty string ────────────────────────────────────────────────────────────

TEST(MessengerHexCodecTest, EncodeEmptyReturnsEmpty)
{
    EXPECT_EQ(hex_encode_schema_hash(""), "");
}

TEST(MessengerHexCodecTest, DecodeEmptyReturnsEmpty)
{
    EXPECT_EQ(hex_decode_schema_hash(""), "");
}

// ── Invalid hex chars → empty ───────────────────────────────────────────────

TEST(MessengerHexCodecTest, DecodeInvalidCharsReturnsEmpty)
{
    // 64 hex-length string but with invalid chars 'g' and 'z'.
    std::string bad(kSchemaHashHexLen, 'g');
    EXPECT_EQ(hex_decode_schema_hash(bad), "");
}

// ── Wrong length → empty ────────────────────────────────────────────────────

TEST(MessengerHexCodecTest, DecodeTooShortReturnsEmpty)
{
    EXPECT_EQ(hex_decode_schema_hash("abcdef"), "");
}

TEST(MessengerHexCodecTest, DecodeTooLongReturnsEmpty)
{
    std::string too_long(kSchemaHashHexLen + 2, 'a');
    EXPECT_EQ(hex_decode_schema_hash(too_long), "");
}

// ── Case-insensitive decode ─────────────────────────────────────────────────

TEST(MessengerHexCodecTest, DecodeUpperCaseWorks)
{
    std::string raw(kSchemaHashBytes, '\0');
    for (size_t i = 0; i < kSchemaHashBytes; ++i)
        raw[i] = static_cast<char>(0xFF - i);

    const std::string hex_lower = hex_encode_schema_hash(raw);

    // Convert to uppercase.
    std::string hex_upper = hex_lower;
    for (auto &c : hex_upper)
    {
        if (c >= 'a' && c <= 'f')
            c = static_cast<char>(c - 'a' + 'A');
    }

    EXPECT_EQ(hex_decode_schema_hash(hex_upper), raw);
}

// ── Known vector ────────────────────────────────────────────────────────────

TEST(MessengerHexCodecTest, KnownVector)
{
    // All zeros → "000...0" (64 hex chars)
    std::string raw(kSchemaHashBytes, '\0');
    const std::string hex = hex_encode_schema_hash(raw);
    EXPECT_EQ(hex.size(), kSchemaHashHexLen);
    EXPECT_EQ(hex, std::string(kSchemaHashHexLen, '0'));
    EXPECT_EQ(hex_decode_schema_hash(hex), raw);

    // All 0xFF → "ffffff...ff"
    std::string raw_ff(kSchemaHashBytes, '\xFF');
    const std::string hex_ff = hex_encode_schema_hash(raw_ff);
    EXPECT_EQ(hex_ff, std::string(kSchemaHashHexLen, 'f'));
    EXPECT_EQ(hex_decode_schema_hash(hex_ff), raw_ff);
}
