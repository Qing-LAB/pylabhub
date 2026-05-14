// test_datahub_schema_loader.cpp
//
// Unit tests for the stateless schema parser surface — HEP-CORE-0034 §2.4 I5.
//
// After Phase 4 demotion, `SchemaLibrary` is a namespace-shell holding only
// static parsers. The runtime authority for schemas lives in `HubState.schemas`
// (HEP-CORE-0034 §2.4 I1+I3+I4); registry-state tests and validator tests have
// been removed because the surfaces they covered no longer exist.
//
// ── Suite layout ────────────────────────────────────────────────────────────
//
//   Suite 1 — `DatahubSchemaParser` (12 plain `TEST` bodies, Pattern 1)
//     Stateless parser tests for `SchemaLibrary::load_from_string` and the
//     C++ `generate_schema_info<T>` macro path.  No `LOGGER_*`, no lifecycle
//     module, no filesystem I/O — pure functions, run in-process.
//
//   Suite 2 — `DatahubSchemaFileLoadTest` (4 `TEST_F` bodies, Pattern 3)
//     File-walker tests for `pylabhub::schema::load_all_from_dirs`
//     (HEP-CORE-0034 §2.4 I2 entry-point).  `load_all_from_dirs` emits
//     `LOGGER_WARN` on invalid-JSON-skip and duplicate-schema_id-skip;
//     Logger is a lifecycle module, so these tests run in subprocess
//     workers per `docs/README/README_testing.md` § "Choosing a test
//     pattern".  Worker bodies live in
//     `workers/datahub_schema_loader_workers.cpp`.
//
// Migrated 2026-05-14: the 4 file-walker `TEST_F`s moved from in-process
// `SetUpTestSuite`-owned `LifecycleGuard` (single module: Logger) to Pattern 3.
// The 12 plain `TEST`s stay in this file unchanged.
//
// Tests NOT covered here:
//   - Registry state, lookup-by-id, reverse-by-hash → see test_hub_state.cpp
//     (HubState.schemas is the runtime authority).
//   - validate_named_schema<T,F> → deleted; broker NACK on REG_REQ is the
//     validator (HEP-CORE-0034 §2.4 I4).

#include "plh_datahub_client.hpp"
#include "test_patterns.h"
#include "utils/schema_loader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>

using namespace pylabhub::schema;

// ============================================================================
// C++ compile-time struct — mirrors kTempRawJson slot layout exactly
// ============================================================================
struct TempRawSlot
{
    double ts;     // float64
    float  value;  // float32
    // 4 bytes tail padding (natural alignment to 8 bytes)
};

PYLABHUB_SCHEMA_BEGIN(TempRawSlot)
    PYLABHUB_SCHEMA_MEMBER(ts)
    PYLABHUB_SCHEMA_MEMBER(value)
PYLABHUB_SCHEMA_END(TempRawSlot)

struct SamplesSlot
{
    double  ts;
    float   samples[8];
};

PYLABHUB_SCHEMA_BEGIN(SamplesSlot)
    PYLABHUB_SCHEMA_MEMBER(ts)
    PYLABHUB_SCHEMA_MEMBER(samples)
PYLABHUB_SCHEMA_END(SamplesSlot)

// ============================================================================
// Test JSON fixtures
// ============================================================================

static constexpr const char kTempRawJson[] = R"json({
  "id":      "lab.sensors.temperature.raw",
  "version": 1,
  "description": "Raw temperature",
  "slot": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  }
})json";

static constexpr const char kSamplesJson[] = R"json({
  "id":      "lab.sensors.samples",
  "version": 1,
  "slot": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",      "type": "float64"},
      {"name": "samples", "type": "float32", "count": 8}
    ]
  }
})json";

static constexpr const char kWithFlexzoneJson[] = R"json({
  "id":      "lab.sensors.calibrated",
  "version": 2,
  "slot": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  },
  "flexzone": {
    "packing": "aligned",
    "fields": [
      {"name": "cal_offset", "type": "float64"},
      {"name": "cal_scale",  "type": "float64"}
    ]
  }
})json";

static constexpr const char kAllTypesJson[] = R"json({
  "id":      "test.all_types",
  "version": 1,
  "slot": {
    "packing": "aligned",
    "fields": [
      {"name": "f32_field", "type": "float32"},
      {"name": "f64_field", "type": "float64"},
      {"name": "i8_field",  "type": "int8"},
      {"name": "u8_field",  "type": "uint8"},
      {"name": "i16_field", "type": "int16"},
      {"name": "u16_field", "type": "uint16"},
      {"name": "i32_field", "type": "int32"},
      {"name": "u32_field", "type": "uint32"},
      {"name": "i64_field", "type": "int64"},
      {"name": "u64_field", "type": "uint64"},
      {"name": "bool_field","type": "bool"},
      {"name": "char_field","type": "char"}
    ]
  }
})json";

// ============================================================================
// Parser tests — `SchemaLibrary::load_from_string` (static, stateless)
// ============================================================================

TEST(DatahubSchemaParser, LoadFromString_BasicParsing)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson);

    EXPECT_EQ(e.schema_id,  "$lab.sensors.temperature.raw.v1");
    EXPECT_EQ(e.version,    1u);
    EXPECT_EQ(e.description,"Raw temperature");

    ASSERT_EQ(e.slot.fields.size(), 2u);
    EXPECT_EQ(e.slot.fields[0].name, "ts");
    EXPECT_EQ(e.slot.fields[0].type, "float64");
    EXPECT_EQ(e.slot.fields[0].count, 1u);
    EXPECT_EQ(e.slot.fields[1].name, "value");
    EXPECT_EQ(e.slot.fields[1].type, "float32");

    EXPECT_FALSE(e.has_flexzone());
}

TEST(DatahubSchemaParser, BLDSStringGeneration)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson);
    EXPECT_EQ(e.slot_info.blds, "ts:f64;value:f32");
}

TEST(DatahubSchemaParser, ArrayFieldBLDS)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kSamplesJson);
    EXPECT_EQ(e.slot_info.blds, "ts:f64;samples:f32[8]");
    ASSERT_EQ(e.slot.fields.size(), 2u);
    EXPECT_EQ(e.slot.fields[1].count, 8u);
}

TEST(DatahubSchemaParser, NaturalAlignmentStructSize)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson);
    EXPECT_EQ(e.slot_info.struct_size, 16u);
}

TEST(DatahubSchemaParser, ArrayFieldStructSize)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kSamplesJson);
    EXPECT_EQ(e.slot_info.struct_size, 40u);
}

TEST(DatahubSchemaParser, HashComputedAndNonZero)
{
    const SchemaEntry             e = SchemaLibrary::load_from_string(kTempRawJson);
    const std::array<uint8_t, 32> zero{};

    EXPECT_NE(e.slot_info.hash, zero) << "slot hash should not be zero";
    EXPECT_EQ(e.flexzone_info.hash, zero) << "flexzone hash should be zero when no flexzone";
}

TEST(DatahubSchemaParser, FlexzoneSchemaInfoPopulated)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kWithFlexzoneJson);

    EXPECT_TRUE(e.has_flexzone());
    EXPECT_EQ(e.schema_id, "$lab.sensors.calibrated.v2");

    EXPECT_EQ(e.flexzone_info.blds, "cal_offset:f64;cal_scale:f64");
    EXPECT_EQ(e.flexzone_info.struct_size, 16u);

    const std::array<uint8_t, 32> zero{};
    EXPECT_NE(e.flexzone_info.hash, zero);
}

TEST(DatahubSchemaParser, AllPrimitiveTypesParse)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kAllTypesJson);
    ASSERT_EQ(e.slot.fields.size(), 12u);

    const std::string &blds = e.slot_info.blds;
    EXPECT_NE(blds.find("f32_field:f32"), std::string::npos);
    EXPECT_NE(blds.find("f64_field:f64"), std::string::npos);
    EXPECT_NE(blds.find("i8_field:i8"),   std::string::npos);
    EXPECT_NE(blds.find("u8_field:u8"),   std::string::npos);
    EXPECT_NE(blds.find("i16_field:i16"), std::string::npos);
    EXPECT_NE(blds.find("u16_field:u16"), std::string::npos);
    EXPECT_NE(blds.find("i32_field:i32"), std::string::npos);
    EXPECT_NE(blds.find("u32_field:u32"), std::string::npos);
    EXPECT_NE(blds.find("i64_field:i64"), std::string::npos);
    EXPECT_NE(blds.find("u64_field:u64"), std::string::npos);
    EXPECT_NE(blds.find("bool_field:b"),  std::string::npos);
    EXPECT_NE(blds.find("char_field:c"),  std::string::npos);

    const std::array<uint8_t, 32> zero{};
    EXPECT_NE(e.slot_info.hash, zero);
}

TEST(DatahubSchemaParser, SchemaIDOverride)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson, "$custom.alias.v7");
    EXPECT_EQ(e.schema_id, "$custom.alias.v7");
    EXPECT_EQ(e.version,   7u);
    EXPECT_EQ(e.slot_info.blds, "ts:f64;value:f32");
}

TEST(DatahubSchemaParser, DeterministicHash)
{
    const SchemaEntry e1 = SchemaLibrary::load_from_string(kTempRawJson);
    const SchemaEntry e2 = SchemaLibrary::load_from_string(kTempRawJson, "$other.name.v1");

    // Different IDs but same fields → same slot hash (HEP-0002 BLDS form).
    EXPECT_EQ(e1.slot_info.hash, e2.slot_info.hash);
    EXPECT_EQ(e1.slot_info.blds, e2.slot_info.blds);
}

TEST(DatahubSchemaParser, DifferentSchemasHaveDifferentHashes)
{
    const SchemaEntry e_temp    = SchemaLibrary::load_from_string(kTempRawJson);
    const SchemaEntry e_samples = SchemaLibrary::load_from_string(kSamplesJson);

    EXPECT_NE(e_temp.slot_info.hash, e_samples.slot_info.hash);
}

// ── Compile-time C++ struct → JSON schema parity ────────────────────────────
//
// The C++ macros (PYLABHUB_SCHEMA_BEGIN/MEMBER/END) and the JSON parser
// (`load_from_string`) produce the same HEP-0002 BLDS canonical form for
// a given field list, hence the same `SchemaInfo::hash`.  This is a
// foundation invariant for SHM-header self-description; it is NOT the
// HEP-0034 wire-form fingerprint (see HEP-CORE-0034 §2.4 I6).

TEST(DatahubSchemaParser, CppStructMatchesJsonSchema)
{
    const SchemaInfo cpp_info = generate_schema_info<TempRawSlot>(
        "TempRawSlot", SchemaVersion{1, 0, 0});
    const SchemaEntry json_entry = SchemaLibrary::load_from_string(kTempRawJson);

    EXPECT_EQ(cpp_info.blds, json_entry.slot_info.blds)
        << "C++ BLDS:  " << cpp_info.blds << "\n"
        << "JSON BLDS: " << json_entry.slot_info.blds;

    EXPECT_EQ(cpp_info.hash, json_entry.slot_info.hash)
        << "Hash mismatch — C++ struct and JSON schema are not structurally equivalent";

    EXPECT_EQ(cpp_info.struct_size, json_entry.slot_info.struct_size);
    EXPECT_EQ(cpp_info.struct_size, sizeof(TempRawSlot));
}

// ============================================================================
// Suite 2 — File-walker tests (Pattern 3; subprocess per TEST_F)
//
// `load_all_from_dirs` emits LOGGER_WARN on invalid-JSON-skip and duplicate-
// schema_id-skip — Logger is a lifecycle module, so these run in workers.
// Worker bodies live in `workers/datahub_schema_loader_workers.cpp`.
// ============================================================================

using pylabhub::tests::IsolatedProcessTest;

class DatahubSchemaFileLoadTest : public IsolatedProcessTest
{
};

TEST_F(DatahubSchemaFileLoadTest, LoadAllFromDirs_SingleFile)
{
    auto w = SpawnWorker(
        "datahub_schema_loader.load_all_from_dirs_single_file");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaFileLoadTest, LoadAllFromDirs_NestedPath)
{
    auto w = SpawnWorker(
        "datahub_schema_loader.load_all_from_dirs_nested_path");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaFileLoadTest, LoadAllFromDirs_InvalidJsonSkipped)
{
    auto w = SpawnWorker(
        "datahub_schema_loader.load_all_from_dirs_invalid_json_skipped");
    ExpectWorkerOk(w);
}

TEST_F(DatahubSchemaFileLoadTest, LoadAllFromDirs_FirstMatchWinsAcrossDirs)
{
    auto w = SpawnWorker(
        "datahub_schema_loader.load_all_from_dirs_first_match_wins_across_dirs");
    ExpectWorkerOk(w);
}
