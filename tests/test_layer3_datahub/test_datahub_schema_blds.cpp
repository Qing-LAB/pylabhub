/**
 * @file test_schema_blds.cpp
 * @brief Layer 3 tests for BLDS schema generation (schema_blds.hpp).
 *
 * CRITICAL for P9.2 schema validation. Tests cover:
 * - BLDSTypeID mapping (fundamental types, arrays, std::atomic)
 * - BLDSBuilder (add_member, build)
 * - SchemaVersion pack/unpack
 * - generate_schema_info (with PYLABHUB_SCHEMA_* macros)
 * - SchemaInfo hash, matches, validate_schema_*
 */
#include "test_patterns.h"
#include "datahub_schema_blds_workers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

using namespace pylabhub::schema;
using namespace pylabhub::tests;

// ============================================================================
// BLDSTypeID - Pure API (no lifecycle)
// ============================================================================

TEST(DatahubSchemaBldsTypeId, FloatingPoint)
{
    EXPECT_STREQ(BLDSTypeID<float>::value, "f32");
    EXPECT_STREQ(BLDSTypeID<double>::value, "f64");
}

TEST(DatahubSchemaBldsTypeId, SignedIntegers)
{
    EXPECT_STREQ(BLDSTypeID<int8_t>::value, "i8");
    EXPECT_STREQ(BLDSTypeID<int16_t>::value, "i16");
    EXPECT_STREQ(BLDSTypeID<int32_t>::value, "i32");
    EXPECT_STREQ(BLDSTypeID<int64_t>::value, "i64");
}

TEST(DatahubSchemaBldsTypeId, UnsignedIntegers)
{
    EXPECT_STREQ(BLDSTypeID<uint8_t>::value, "u8");
    EXPECT_STREQ(BLDSTypeID<uint16_t>::value, "u16");
    EXPECT_STREQ(BLDSTypeID<uint32_t>::value, "u32");
    EXPECT_STREQ(BLDSTypeID<uint64_t>::value, "u64");
}

TEST(DatahubSchemaBldsTypeId, BoolAndChar)
{
    EXPECT_STREQ(BLDSTypeID<bool>::value, "b");
    EXPECT_STREQ(BLDSTypeID<char>::value, "c");
}

TEST(DatahubSchemaBldsTypeId, AtomicUsesUnderlyingType)
{
    EXPECT_STREQ(BLDSTypeID<std::atomic<uint64_t>>::value, "u64");
    EXPECT_STREQ(BLDSTypeID<std::atomic<int32_t>>::value, "i32");
}

TEST(DatahubSchemaBldsTypeId, ArrayOfScalar)
{
    EXPECT_EQ(BLDSTypeID<float[4]>::value(), "f32[4]");
    EXPECT_EQ(BLDSTypeID<int32_t[8]>::value(), "i32[8]");
}

TEST(DatahubSchemaBldsTypeId, CharArrayIsString)
{
    EXPECT_EQ(BLDSTypeID<char[64]>::value(), "c[64]");
}

TEST(DatahubSchemaBldsTypeId, StdArray)
{
    using FloatArray4 = std::array<float, 4>;
    using Uint8Array32 = std::array<uint8_t, 32>;
    EXPECT_EQ(BLDSTypeID<FloatArray4>::value(), "f32[4]");
    EXPECT_EQ(BLDSTypeID<Uint8Array32>::value(), "u8[32]");
}

// ============================================================================
// BLDSBuilder - Pure API (no lifecycle)
// ============================================================================

TEST(DatahubSchemaBldsBuilder, SingleMember)
{
    BLDSBuilder b;
    b.add_member("foo", "u64");
    EXPECT_EQ(b.build(), "foo:u64");
}

TEST(DatahubSchemaBldsBuilder, MultipleMembers)
{
    BLDSBuilder b;
    b.add_member("foo", "u64");
    b.add_member("bar", "f32");
    b.add_member("baz", "i32");
    EXPECT_EQ(b.build(), "foo:u64;bar:f32;baz:i32");
}

TEST(DatahubSchemaBldsBuilder, MemberWithOffsetAndSize)
{
    BLDSBuilder b;
    b.add_member("magic", "u32", 0, 4);
    b.add_member("version", "u16", 4, 2);
    EXPECT_EQ(b.build(), "magic:u32@0:4;version:u16@4:2");
}

// ============================================================================
// SchemaVersion - Pure API (no lifecycle)
// ============================================================================

TEST(DatahubSchemaBldsVersion, ToString)
{
    SchemaVersion v{1, 2, 3};
    EXPECT_EQ(v.to_string(), "1.2.3");
}

TEST(DatahubSchemaBldsVersion, PackUnpackRoundTrip)
{
    SchemaVersion v{1, 2, 3};
    uint32_t packed = v.pack();
    SchemaVersion u = SchemaVersion::unpack(packed);
    EXPECT_EQ(u.major, v.major);
    EXPECT_EQ(u.minor, v.minor);
    EXPECT_EQ(u.patch, v.patch);
}

TEST(DatahubSchemaBldsVersion, PackUnpackMaxValues)
{
    // major: 10 bits (0x3FF), minor: 10 bits, patch: 12 bits (0xFFF)
    SchemaVersion v{1023, 1023, 4095};
    uint32_t packed = v.pack();
    SchemaVersion u = SchemaVersion::unpack(packed);
    EXPECT_EQ(u.major, 1023u);
    EXPECT_EQ(u.minor, 1023u);
    EXPECT_EQ(u.patch, 4095u);
}

TEST(DatahubSchemaBldsVersion, PackUnpackZero)
{
    SchemaVersion v{0, 0, 0};
    uint32_t packed = v.pack();
    EXPECT_EQ(packed, 0u);
    SchemaVersion u = SchemaVersion::unpack(0);
    EXPECT_EQ(u.major, 0u);
    EXPECT_EQ(u.minor, 0u);
    EXPECT_EQ(u.patch, 0u);
}

// ============================================================================
// generate_schema_info + SchemaInfo (requires crypto lifecycle — isolated process)
// ============================================================================

class DatahubSchemaBldsInfoTest : public IsolatedProcessTest
{
};

TEST_F(DatahubSchemaBldsInfoTest, GenerateSchemaInfo_SetsNameVersionSize)
{
    auto w = SpawnWorker("schema_blds.schema_info_name_version_size");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaBldsInfoTest, GenerateSchemaInfo_BldsFormat)
{
    auto w = SpawnWorker("schema_blds.schema_info_blds_format");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaBldsInfoTest, GenerateSchemaInfo_HashIsDeterministic)
{
    auto w = SpawnWorker("schema_blds.schema_info_hash_deterministic");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaBldsInfoTest, GenerateSchemaInfo_DifferentStructDifferentHash)
{
    auto w = SpawnWorker("schema_blds.schema_info_different_hash");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaBldsInfoTest, SchemaInfo_Matches)
{
    auto w = SpawnWorker("schema_blds.schema_info_matches");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaBldsInfoTest, SchemaInfo_MatchesHash)
{
    auto w = SpawnWorker("schema_blds.schema_info_matches_hash");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaBldsInfoTest, ValidateSchemaMatch_SameSchema_DoesNotThrow)
{
    auto w = SpawnWorker("schema_blds.validate_match_same_ok");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaBldsInfoTest, ValidateSchemaMatch_DifferentSchema_Throws)
{
    auto w = SpawnWorker("schema_blds.validate_match_diff_throws");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaBldsInfoTest, ValidateSchemaHash_Matching_DoesNotThrow)
{
    auto w = SpawnWorker("schema_blds.validate_hash_match_ok");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaBldsInfoTest, ValidateSchemaHash_Mismatch_Throws)
{
    auto w = SpawnWorker("schema_blds.validate_hash_mismatch_throws");
    ExpectWorkerOk(w);
}
