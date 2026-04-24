// test_datahub_schema_library.cpp
//
// Unit tests for SchemaLibrary (HEP-CORE-0016 Phase 1).
//
// SchemaLibrary is a plain utility class (no SHM, no ZMQ, no lifecycle),
// so these are direct GTest tests — no subprocess worker pattern needed.
//
// Test suite: DatahubSchemaLibraryTest
// Tests: 17 (15 JSON-only + 2 compile-time ↔ runtime matching)

#include "plh_datahub_client.hpp"
#include "utils/schema_library.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace pylabhub::schema;

// ============================================================================
// C++ compile-time struct — mirrors kTempRawJson slot layout exactly
// ============================================================================
//
// JSON:
//   {"name": "ts",    "type": "float64"}   →  double ts;
//   {"name": "value", "type": "float32"}   →  float  value;
//
// Natural layout: ts@0:8, value@8:4, pad@12:4 → sizeof = 16
//

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

// A second struct with an array — mirrors kSamplesJson
struct SamplesSlot
{
    double  ts;           // float64
    float   samples[8];   // float32[8]
};

PYLABHUB_SCHEMA_BEGIN(SamplesSlot)
    PYLABHUB_SCHEMA_MEMBER(ts)
    PYLABHUB_SCHEMA_MEMBER(samples)
PYLABHUB_SCHEMA_END(SamplesSlot)

// ── Phase 2 test structs ─────────────────────────────────────────────────────

// Same size as TempRawSlot (16 bytes) but different field order → different hash
struct WrongOrderSlot
{
    float  value; // float32 @ 0:4
    // 4 bytes padding (natural alignment to 8)
    double ts;    // float64 @ 8:8
    // total: 16 bytes
};

PYLABHUB_SCHEMA_BEGIN(WrongOrderSlot)
    PYLABHUB_SCHEMA_MEMBER(value)
    PYLABHUB_SCHEMA_MEMBER(ts)
PYLABHUB_SCHEMA_END(WrongOrderSlot)

// Different size (4 bytes) — size check must catch this
struct TinySlot
{
    float v; // float32
};

// FlexZone struct (16 bytes) — matches kWithFlexzoneJson flexzone layout
struct CalibFZ
{
    double cal_offset; // float64
    double cal_scale;  // float64
};

PYLABHUB_SCHEMA_BEGIN(CalibFZ)
    PYLABHUB_SCHEMA_MEMBER(cal_offset)
    PYLABHUB_SCHEMA_MEMBER(cal_scale)
PYLABHUB_SCHEMA_END(CalibFZ)

// ============================================================================
// Helpers — minimal JSON schema strings
// ============================================================================

// A two-field schema: float64 ts + float32 value
// struct layout: ts@0:8, value@8:4, pad@12:4 → total 16 bytes
static constexpr const char kTempRawJson[] = R"json({
  "id":      "lab.sensors.temperature.raw",
  "version": 1,
  "description": "Raw temperature",
  "slot": {
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  }
})json";

// A schema with an array field: float32 samples[8]
static constexpr const char kSamplesJson[] = R"json({
  "id":      "lab.sensors.samples",
  "version": 1,
  "slot": {
    "fields": [
      {"name": "ts",      "type": "float64"},
      {"name": "samples", "type": "float32", "count": 8}
    ]
  }
})json";

// A schema with a flexzone
static constexpr const char kWithFlexzoneJson[] = R"json({
  "id":      "lab.sensors.calibrated",
  "version": 2,
  "slot": {
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  },
  "flexzone": {
    "fields": [
      {"name": "cal_offset", "type": "float64"},
      {"name": "cal_scale",  "type": "float64"}
    ]
  }
})json";

// A schema that uses many primitive types
static constexpr const char kAllTypesJson[] = R"json({
  "id":      "test.all_types",
  "version": 1,
  "slot": {
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
// Test fixture
// ============================================================================

class DatahubSchemaLibraryTest : public ::testing::Test
{
protected:
    // Use empty search dirs — tests register schemas in-memory or load from string
    DatahubSchemaLibraryTest() : lib_(std::vector<std::string>{}) {}

    SchemaLibrary lib_;
};

// ============================================================================
// Test 1: load_from_string — basic parsing
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, LoadFromString_BasicParsing)
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

    // No flexzone declared
    EXPECT_FALSE(e.has_flexzone());
}

// ============================================================================
// Test 2: BLDS string generation
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, BLDSStringGeneration)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson);

    // Expected BLDS: "ts:f64;value:f32"
    EXPECT_EQ(e.slot_info.blds, "ts:f64;value:f32");
}

// ============================================================================
// Test 3: array field BLDS
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, ArrayFieldBLDS)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kSamplesJson);

    // Expected BLDS: "ts:f64;samples:f32[8]"
    EXPECT_EQ(e.slot_info.blds, "ts:f64;samples:f32[8]");

    ASSERT_EQ(e.slot.fields.size(), 2u);
    EXPECT_EQ(e.slot.fields[1].count, 8u);
}

// ============================================================================
// Test 4: natural-alignment struct size
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, NaturalAlignmentStructSize)
{
    // ts: float64 → 8 bytes @ offset 0
    // value: float32 → 4 bytes @ offset 8
    // tail padding to align to max(8,4)=8: 4 bytes
    // Total: 16 bytes
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson);
    EXPECT_EQ(e.slot_info.struct_size, 16u);
}

// ============================================================================
// Test 5: array field struct size
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, ArrayFieldStructSize)
{
    // ts: float64 → 8 bytes @ offset 0
    // samples: float32[8] → 32 bytes @ offset 8 (align 4 ≤ current 8 → no gap)
    // max alignment = 8; total 40 → aligned to 8 → 40 bytes
    const SchemaEntry e = SchemaLibrary::load_from_string(kSamplesJson);
    EXPECT_EQ(e.slot_info.struct_size, 40u);
}

// ============================================================================
// Test 6: hash is computed and non-zero
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, HashComputedAndNonZero)
{
    const SchemaEntry          e    = SchemaLibrary::load_from_string(kTempRawJson);
    const std::array<uint8_t, 32> zero{};

    EXPECT_NE(e.slot_info.hash, zero)  << "slot hash should not be zero";
    // No flexzone → flexzone hash is zero
    EXPECT_EQ(e.flexzone_info.hash, zero) << "flexzone hash should be zero when no flexzone";
}

// ============================================================================
// Test 7: flexzone SchemaInfo is populated
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, FlexzoneSchemaInfoPopulated)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kWithFlexzoneJson);

    EXPECT_TRUE(e.has_flexzone());
    EXPECT_EQ(e.schema_id, "$lab.sensors.calibrated.v2");

    // flexzone: cal_offset(f64) + cal_scale(f64) → 16 bytes, no padding
    EXPECT_EQ(e.flexzone_info.blds, "cal_offset:f64;cal_scale:f64");
    EXPECT_EQ(e.flexzone_info.struct_size, 16u);

    const std::array<uint8_t, 32> zero{};
    EXPECT_NE(e.flexzone_info.hash, zero);
}

// ============================================================================
// Test 8: register + forward lookup (get)
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, RegisterAndGetForwardLookup)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson);
    lib_.register_schema(e);

    const auto found = lib_.get("$lab.sensors.temperature.raw.v1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->schema_id, "$lab.sensors.temperature.raw.v1");
    EXPECT_EQ(found->slot_info.blds, "ts:f64;value:f32");
}

// ============================================================================
// Test 9: get unknown returns nullopt
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, GetUnknownReturnsNullopt)
{
    const auto found = lib_.get("$does.not.exist.v99");
    EXPECT_FALSE(found.has_value());
}

// ============================================================================
// Test 10: identify — reverse hash lookup
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, IdentifyReverseLookup)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson);
    lib_.register_schema(e);

    const auto id = lib_.identify(e.slot_info.hash);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "$lab.sensors.temperature.raw.v1");
}

// ============================================================================
// Test 11: list returns all registered IDs
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, ListReturnsAllIDs)
{
    lib_.register_schema(SchemaLibrary::load_from_string(kTempRawJson));
    lib_.register_schema(SchemaLibrary::load_from_string(kSamplesJson));
    lib_.register_schema(SchemaLibrary::load_from_string(kWithFlexzoneJson));

    const auto ids = lib_.list();
    EXPECT_EQ(ids.size(), 3u);

    const auto has = [&](const std::string &id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    EXPECT_TRUE(has("$lab.sensors.temperature.raw.v1"));
    EXPECT_TRUE(has("$lab.sensors.samples.v1"));
    EXPECT_TRUE(has("$lab.sensors.calibrated.v2"));
}

// ============================================================================
// Test 12: all primitive types parse and produce non-zero hash
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, AllPrimitiveTypesParse)
{
    const SchemaEntry e = SchemaLibrary::load_from_string(kAllTypesJson);
    ASSERT_EQ(e.slot.fields.size(), 12u);

    // BLDS should contain all 12 tokens
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

// ============================================================================
// Test 13: schema_id override in load_from_string
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, SchemaIDOverride)
{
    // Override the id — useful for in-memory registration with a custom alias
    const SchemaEntry e = SchemaLibrary::load_from_string(kTempRawJson, "$custom.alias.v7");
    EXPECT_EQ(e.schema_id, "$custom.alias.v7");
    EXPECT_EQ(e.version,   7u);
    // BLDS is from the JSON slot fields regardless of the id override
    EXPECT_EQ(e.slot_info.blds, "ts:f64;value:f32");
}

// ============================================================================
// Test 14: identical field list → identical hash (deterministic)
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, DeterministicHash)
{
    const SchemaEntry e1 = SchemaLibrary::load_from_string(kTempRawJson);
    const SchemaEntry e2 = SchemaLibrary::load_from_string(kTempRawJson, "$other.name.v1");

    // Different IDs but same fields → same slot hash
    EXPECT_EQ(e1.slot_info.hash, e2.slot_info.hash);
    EXPECT_EQ(e1.slot_info.blds, e2.slot_info.blds);
}

// ============================================================================
// Test 15: different schemas have different hashes
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, DifferentSchemasHaveDifferentHashes)
{
    const SchemaEntry e_temp    = SchemaLibrary::load_from_string(kTempRawJson);
    const SchemaEntry e_samples = SchemaLibrary::load_from_string(kSamplesJson);

    EXPECT_NE(e_temp.slot_info.hash, e_samples.slot_info.hash);
}

// ============================================================================
// Test 16: compile-time C++ struct → matches JSON schema (hash + size agree)
//
// A C++ struct registered with PYLABHUB_SCHEMA_BEGIN/MEMBER/END macros
// produces the same BLDS and hash as the equivalent JSON schema.
// This verifies that schema_blds.hpp (compile-time) and schema_library.cpp
// (runtime JSON) use the same canonical representation.
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, CppStructMatchesJsonSchema)
{
    // Compile-time: generate SchemaInfo from TempRawSlot via macros
    const SchemaInfo cpp_info = generate_schema_info<TempRawSlot>(
        "TempRawSlot", SchemaVersion{1, 0, 0});

    // Runtime: parse the equivalent JSON schema
    const SchemaEntry json_entry = SchemaLibrary::load_from_string(kTempRawJson);

    // BLDS strings must be identical
    EXPECT_EQ(cpp_info.blds, json_entry.slot_info.blds)
        << "C++ BLDS:  " << cpp_info.blds << "\n"
        << "JSON BLDS: " << json_entry.slot_info.blds;

    // Hashes must match (checksum = primary identity)
    EXPECT_EQ(cpp_info.hash, json_entry.slot_info.hash)
        << "Hash mismatch — C++ struct and JSON schema are not structurally equivalent";

    // Struct sizes must agree (both use natural alignment)
    EXPECT_EQ(cpp_info.struct_size, json_entry.slot_info.struct_size);
    EXPECT_EQ(cpp_info.struct_size, sizeof(TempRawSlot));
}

// ============================================================================
// Test 17: reverse lookup — C++ hash → named schema ID
//
// Simulates the broker annotation flow: a producer registers an unnamed schema
// (its BLAKE2b hash is sent in REG_REQ).  The schema library can identify
// which named schema matches that hash and return its ID.
// This is the "unnamed → named" annotation path from HEP-CORE-0016 §7.
// ============================================================================

TEST_F(DatahubSchemaLibraryTest, CppHashIdentifiesNamedSchema)
{
    // Register the named JSON schema in the library
    lib_.register_schema(SchemaLibrary::load_from_string(kTempRawJson));

    // Compute the C++ struct hash independently
    const SchemaInfo cpp_info = generate_schema_info<TempRawSlot>(
        "TempRawSlot", SchemaVersion{1, 0, 0});

    // identify() must return the named schema ID for the C++ hash
    const auto id = lib_.identify(cpp_info.hash);
    ASSERT_TRUE(id.has_value())
        << "Library did not identify C++ struct hash as a named schema";
    EXPECT_EQ(*id, "$lab.sensors.temperature.raw.v1");

    // Also verify the array struct variant independently
    lib_.register_schema(SchemaLibrary::load_from_string(kSamplesJson));
    const SchemaInfo cpp_samples = generate_schema_info<SamplesSlot>(
        "SamplesSlot", SchemaVersion{1, 0, 0});
    const auto id2 = lib_.identify(cpp_samples.hash);
    ASSERT_TRUE(id2.has_value());
    EXPECT_EQ(*id2, "$lab.sensors.samples.v1");
}

// ============================================================================
// Phase 2 tests — validate_named_schema<DataT, FlexT>(schema_id, lib)
// (HEP-CORE-0016 Phase 2: C++ Integration)
//
// Tests use an in-memory SchemaLibrary (register_schema) to avoid file I/O
// and env-var setup.  validate_named_schema_from_env() is tested via env-path
// round-trip only in the matching-schema case.
// ============================================================================

class DatahubSchemaPhase2Test : public ::testing::Test
{
protected:
    DatahubSchemaPhase2Test() : lib_(std::vector<std::string>{})
    {
        lib_.register_schema(SchemaLibrary::load_from_string(kTempRawJson));
        lib_.register_schema(SchemaLibrary::load_from_string(kSamplesJson));
        lib_.register_schema(SchemaLibrary::load_from_string(kWithFlexzoneJson));
    }

    SchemaLibrary lib_;
};

// ── Test 18: matching struct → no exception ───────────────────────────────────

TEST_F(DatahubSchemaPhase2Test, MatchingStruct_NoThrow)
{
    // TempRawSlot is registered with PYLABHUB_SCHEMA macros and matches kTempRawJson.
    EXPECT_NO_THROW(
        (pylabhub::schema::validate_named_schema<TempRawSlot>(
            "$lab.sensors.temperature.raw.v1", lib_)));
}

// ── Test 19: empty schema_id → no check, no exception ────────────────────────

TEST_F(DatahubSchemaPhase2Test, EmptySchemaId_NoCheck)
{
    // Empty ID → validation is a no-op regardless of the struct type.
    EXPECT_NO_THROW(
        (pylabhub::schema::validate_named_schema<TinySlot>("", lib_)));
}

// ── Test 20: unknown schema ID → throws SchemaValidationException ─────────────

TEST_F(DatahubSchemaPhase2Test, UnknownId_Throws)
{
    EXPECT_THROW(
        (pylabhub::schema::validate_named_schema<TempRawSlot>(
            "$does.not.exist.v99", lib_)),
        pylabhub::schema::SchemaValidationException);
}

// ── Test 21: slot size mismatch → throws ─────────────────────────────────────

TEST_F(DatahubSchemaPhase2Test, SlotSizeMismatch_Throws)
{
    // TinySlot is 4 bytes; kTempRawJson expects 16 bytes.
    // No PYLABHUB_SCHEMA macros on TinySlot → size-only check.
    EXPECT_THROW(
        (pylabhub::schema::validate_named_schema<TinySlot>(
            "$lab.sensors.temperature.raw.v1", lib_)),
        pylabhub::schema::SchemaValidationException);
}

// ── Test 22: slot BLDS hash mismatch (same size, wrong field order) → throws ──

TEST_F(DatahubSchemaPhase2Test, SlotHashMismatch_Throws)
{
    // WrongOrderSlot is 16 bytes (same as TempRawSlot) but fields are in different
    // order → BLDS = "value:f32;ts:f64" ≠ "ts:f64;value:f32" → hash mismatch.
    // WrongOrderSlot IS registered with PYLABHUB_SCHEMA macros → hash check fires.
    EXPECT_THROW(
        (pylabhub::schema::validate_named_schema<WrongOrderSlot>(
            "$lab.sensors.temperature.raw.v1", lib_)),
        pylabhub::schema::SchemaValidationException);
}

// ── Test 23: flexzone size mismatch → throws ──────────────────────────────────

TEST_F(DatahubSchemaPhase2Test, FlexzoneSizeMismatch_Throws)
{
    // kWithFlexzoneJson flexzone: cal_offset(f64) + cal_scale(f64) = 16 bytes.
    // TinySlot as FlexT: sizeof=4 → mismatch.
    EXPECT_THROW(
        (pylabhub::schema::validate_named_schema<TempRawSlot, TinySlot>(
            "$lab.sensors.calibrated.v2", lib_)),
        pylabhub::schema::SchemaValidationException);
}

// ── Test 24: matching slot + matching flexzone → no exception ─────────────────

TEST_F(DatahubSchemaPhase2Test, MatchingFlexzone_NoThrow)
{
    // kWithFlexzoneJson slot: ts(f64) + value(f32) = 16 bytes (same as TempRawSlot).
    // kWithFlexzoneJson flexzone: cal_offset(f64) + cal_scale(f64) = 16 bytes (= CalibFZ).
    // Both registered with PYLABHUB_SCHEMA macros → hash check fires on both.
    EXPECT_NO_THROW(
        (pylabhub::schema::validate_named_schema<TempRawSlot, CalibFZ>(
            "$lab.sensors.calibrated.v2", lib_)));
}

// ============================================================================
// DatahubSchemaFileLoadTest — file-based loading via load_all()
// ============================================================================

class DatahubSchemaFileLoadTest : public ::testing::Test
{
protected:
    std::filesystem::path tmpdir_;

    void SetUp() override
    {
        tmpdir_ = std::filesystem::temp_directory_path() /
                  ("plh_schema_file_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(tmpdir_);
    }

    void TearDown() override
    {
        try
        {
            if (std::filesystem::exists(tmpdir_))
                std::filesystem::remove_all(tmpdir_);
        }
        catch (...)
        {
        }
    }

    /// Write a JSON file at the given path relative to tmpdir_.
    void write_json(const std::filesystem::path &relative, const std::string &content)
    {
        auto full_path = tmpdir_ / relative;
        std::filesystem::create_directories(full_path.parent_path());
        std::ofstream ofs(full_path);
        ofs << content;
    }
};

TEST_F(DatahubSchemaFileLoadTest, LoadFromDir_SingleFile)
{
    // JSON format: "id" (base ID), "version", "slot" with "fields"
    const std::string schema_json = R"({
        "id": "test.simple",
        "version": 1,
        "slot": {
            "fields": [
                {"name": "value", "type": "float32"}
            ]
        }
    })";
    write_json("test.simple.v1.json", schema_json);

    pylabhub::schema::SchemaLibrary lib({tmpdir_.string()});
    size_t loaded = lib.load_all();
    EXPECT_GE(loaded, 1u) << "Expected at least 1 schema loaded from directory";

    auto entry = lib.get("$test.simple.v1");
    ASSERT_TRUE(entry.has_value()) << "Schema 'test.simple@1' not found after load_all()";
    EXPECT_EQ(entry->schema_id, "$test.simple.v1");
}

TEST_F(DatahubSchemaFileLoadTest, LoadFromDir_NestedPath)
{
    // Nested directory structure: lab/sensors/temperature.raw.v1.json
    const std::string schema_json = R"({
        "id": "lab.sensors.temperature.raw",
        "version": 1,
        "slot": {
            "fields": [
                {"name": "ts", "type": "uint64"},
                {"name": "temperature", "type": "float64"}
            ]
        }
    })";
    write_json(std::filesystem::path("lab") / "sensors" / "temperature.raw.v1.json", schema_json);

    pylabhub::schema::SchemaLibrary lib({tmpdir_.string()});
    size_t loaded = lib.load_all();
    EXPECT_GE(loaded, 1u);

    auto entry = lib.get("$lab.sensors.temperature.raw.v1");
    ASSERT_TRUE(entry.has_value()) << "Nested schema not found after load_all()";
}

TEST_F(DatahubSchemaFileLoadTest, LoadFromDir_InvalidJson_Skipped)
{
    // One valid, one malformed
    const std::string valid_json = R"({
        "id": "test.valid",
        "version": 1,
        "slot": {
            "fields": [{"name": "x", "type": "int32"}]
        }
    })";
    write_json("valid.json", valid_json);
    write_json("broken.json", "{ this is not valid JSON }}}");

    pylabhub::schema::SchemaLibrary lib({tmpdir_.string()});
    size_t loaded = lib.load_all();
    EXPECT_GE(loaded, 1u) << "Valid schema should still load despite broken JSON file";

    auto valid = lib.get("$test.valid.v1");
    EXPECT_TRUE(valid.has_value()) << "Valid schema should be loadable";
}
