/**
 * @file test_schema_validation.cpp
 * @brief Tests for schema parsing, validation, and size computation.
 *
 * Covers parse_schema_json() error paths, compute_field_layout() edge cases,
 * and compute_schema_size() correctness for all field types.
 */
#include "utils/schema_utils.hpp"
#include "utils/schema_field_layout.hpp"

#include <cstring>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::hub;
using json = nlohmann::json;

// ============================================================================
// Helpers
// ============================================================================

static json make_field(const std::string &name, const std::string &type, uint32_t count = 1,
                       uint32_t length = 0)
{
    json f;
    f["name"] = name;
    f["type"] = type;
    if (count != 1)
        f["count"] = count;
    if (length != 0)
        f["length"] = length;
    return f;
}

static json make_schema(std::initializer_list<json> fields, const std::string &packing = "aligned")
{
    json s;
    s["fields"] = json::array();
    s["packing"] = packing;
    for (auto &f : fields)
        s["fields"].push_back(f);
    return s;
}

// ============================================================================
// parse_schema_json — valid paths
// ============================================================================

TEST(SchemaValidationTest, ParseValid_SingleNumericField)
{
    auto s = make_schema({make_field("value", "float64")});
    auto spec = parse_schema_json(s);
    EXPECT_TRUE(spec.has_schema);
    ASSERT_EQ(spec.fields.size(), 1u);
    EXPECT_EQ(spec.fields[0].name, "value");
    EXPECT_EQ(spec.fields[0].type_str, "float64");
    EXPECT_EQ(spec.fields[0].count, 1u);
}

TEST(SchemaValidationTest, ParseValid_MultipleFields)
{
    auto s = make_schema({make_field("ts", "float64"), make_field("val", "int32")});
    auto spec = parse_schema_json(s);
    EXPECT_EQ(spec.fields.size(), 2u);
}

TEST(SchemaValidationTest, ParseValid_ArrayField)
{
    auto s = make_schema({make_field("data", "float32", 4)});
    auto spec = parse_schema_json(s);
    ASSERT_EQ(spec.fields.size(), 1u);
    EXPECT_EQ(spec.fields[0].count, 4u);
}

TEST(SchemaValidationTest, ParseValid_StringField)
{
    auto s = make_schema({make_field("label", "string", 1, 32)});
    auto spec = parse_schema_json(s);
    ASSERT_EQ(spec.fields.size(), 1u);
    EXPECT_EQ(spec.fields[0].type_str, "string");
    EXPECT_EQ(spec.fields[0].length, 32u);
}

TEST(SchemaValidationTest, ParseValid_PackedPacking)
{
    auto s = make_schema({make_field("x", "int32")}, "packed");
    auto spec = parse_schema_json(s);
    EXPECT_EQ(spec.packing, "packed");
}

// ============================================================================
// parse_schema_json — error paths
//
// Each test pins both the exception type AND a substring of the expected
// message (audit §1.1) so a regression where input A triggers error
// path B's throw is caught with a clear diagnostic.  Production has 9
// distinct runtime_error throw sites in `parse_schema_json` (see
// schema_utils.hpp:44-98); type alone is not path-discriminating.
//
// History note: prior to commit `<this>`, this file's
// EmptyFieldsArray / MissingFieldName / MissingFieldType tests omitted
// `"packing"` in the input, which caused all three to actually fire
// the packing-required error path rather than the named scenario.  The
// tests passed type-wise but verified the wrong invariant — exactly
// the silent-failure mode this audit was set up to catch.  Fixed by
// adding `"packing": "aligned"` to those inputs so each test exercises
// the path its name claims.
// ============================================================================

namespace
{
/// Shared helper: invoke `parse_schema_json(s)` and assert it throws a
/// `std::runtime_error` whose what() contains @p needle.  Path-pinned.
void expect_parse_error(const json &s, std::string_view needle)
{
    bool threw = false;
    std::string msg;
    try
    {
        (void)parse_schema_json(s);
    }
    catch (const std::runtime_error &e)
    {
        threw = true;
        msg = e.what();
    }
    EXPECT_TRUE(threw) << "parse_schema_json must throw runtime_error";
    EXPECT_NE(msg.find(needle), std::string::npos)
        << "wrong error path; expected substring '" << needle << "', got: " << msg;
}
} // namespace

TEST(SchemaValidationTest, ParseError_EmptyFieldsArray)
{
    json s;
    s["packing"] = "aligned";
    s["fields"] = json::array();
    expect_parse_error(s, "'fields' array must not be empty");
}

TEST(SchemaValidationTest, ParseError_MissingFieldsKey)
{
    json s;
    s["packing"] = "aligned";
    expect_parse_error(s, "ctypes mode requires a 'fields' array");
}

TEST(SchemaValidationTest, ParseError_MissingFieldName)
{
    json s;
    s["packing"] = "aligned";
    s["fields"] = json::array();
    json f;
    f["type"] = "int32";
    // No "name" key
    s["fields"].push_back(f);
    expect_parse_error(s, "each field must have a string 'name'");
}

TEST(SchemaValidationTest, ParseError_MissingFieldType)
{
    json s;
    s["packing"] = "aligned";
    s["fields"] = json::array();
    json f;
    f["name"] = "x";
    // No "type" key
    s["fields"].push_back(f);
    expect_parse_error(s, "missing 'type'");
}

TEST(SchemaValidationTest, ParseError_InvalidTypeStr)
{
    auto s = make_schema({make_field("x", "complex128")});
    expect_parse_error(s, "unknown type 'complex128'");
}

TEST(SchemaValidationTest, ParseError_CountZero)
{
    json s = make_schema({make_field("x", "int32")});
    s["fields"][0]["count"] = 0;
    expect_parse_error(s, "'count' = 0");
}

TEST(SchemaValidationTest, ParseError_StringLengthZero)
{
    auto s = make_schema({make_field("label", "string", 1, 0)});
    expect_parse_error(s, "requires 'length' > 0");
}

TEST(SchemaValidationTest, ParseError_BytesLengthZero)
{
    auto s = make_schema({make_field("data", "bytes", 1, 0)});
    expect_parse_error(s, "requires 'length' > 0");
}

TEST(SchemaValidationTest, ParseError_ExposeAsPresent)
{
    json s;
    s["expose_as"] = "SomeType";
    s["fields"] = json::array({make_field("x", "int32")});
    expect_parse_error(s, "expose_as is no longer supported");
}

TEST(SchemaValidationTest, ParseError_InvalidPacking)
{
    auto s = make_schema({make_field("x", "int32")}, "natural");
    expect_parse_error(s, "'packing' must be 'aligned' or 'packed'");
}

// ============================================================================
// compute_schema_size — all 13 field types
// ============================================================================

TEST(SchemaValidationTest, SchemaSize_AllNumericTypes)
{
    // Each numeric type should produce a non-zero size.
    const char *types[] = {"bool",   "int8",  "uint8",  "int16",   "uint16", "int32",
                           "uint32", "int64", "uint64", "float32", "float64"};
    for (const char *t : types)
    {
        auto s = make_schema({make_field("v", t)});
        auto spec = parse_schema_json(s);
        size_t sz = compute_schema_size(spec, "aligned");
        EXPECT_GT(sz, 0u) << "Type " << t << " should produce non-zero size";
    }
}

TEST(SchemaValidationTest, SchemaSize_StringField)
{
    auto s = make_schema({make_field("label", "string", 1, 64)});
    auto spec = parse_schema_json(s);
    EXPECT_EQ(compute_schema_size(spec, "aligned"), 64u);
}

TEST(SchemaValidationTest, SchemaSize_BytesField)
{
    auto s = make_schema({make_field("data", "bytes", 1, 128)});
    auto spec = parse_schema_json(s);
    EXPECT_EQ(compute_schema_size(spec, "aligned"), 128u);
}

TEST(SchemaValidationTest, SchemaSize_ArrayField)
{
    // float32[4] = 4 * 4 = 16 bytes
    auto s = make_schema({make_field("data", "float32", 4)});
    auto spec = parse_schema_json(s);
    EXPECT_EQ(compute_schema_size(spec, "aligned"), 16u);
}

TEST(SchemaValidationTest, SchemaSize_PackedVsAligned)
{
    // bool(1B) + int32(4B): aligned = 1+3pad+4 = 8; packed = 1+4 = 5
    auto s = make_schema({make_field("flag", "bool"), make_field("val", "int32")});
    auto spec = parse_schema_json(s);
    EXPECT_EQ(compute_schema_size(spec, "aligned"), 8u);
    EXPECT_EQ(compute_schema_size(spec, "packed"), 5u);
}

TEST(SchemaValidationTest, SchemaSize_EmptySpec_ReturnsZero)
{
    SchemaSpec spec;
    spec.has_schema = false;
    EXPECT_EQ(compute_schema_size(spec, "aligned"), 0u);
}

// ============================================================================
// to_field_descs — conversion correctness
// ============================================================================

TEST(SchemaValidationTest, ToFieldDescs_PreservesTypeCountLength)
{
    std::vector<FieldDef> fields = {
        {"ts", "float64", 1, 0}, {"data", "float32", 4, 0}, {"label", "string", 1, 32}};
    auto descs = to_field_descs(fields);
    ASSERT_EQ(descs.size(), 3u);
    EXPECT_EQ(descs[0].type_str, "float64");
    EXPECT_EQ(descs[0].count, 1u);
    EXPECT_EQ(descs[1].type_str, "float32");
    EXPECT_EQ(descs[1].count, 4u);
    EXPECT_EQ(descs[2].type_str, "string");
    EXPECT_EQ(descs[2].length, 32u);
}

// ============================================================================
// HEP-CORE-0034 §6.3 — packing is part of the fingerprint
// ============================================================================

TEST(SchemaValidationTest, ParseError_MissingPacking)
{
    // HEP-CORE-0034 §6.2 — packing is required, no silent default
    json s;
    s["fields"] = json::array({make_field("x", "int32")});
    // packing intentionally absent
    expect_parse_error(s, "'packing' field is required");
}

TEST(SchemaValidationTest, FingerprintIncludesPacking_AlignedVsPackedDistinct)
{
    // Identical fields, different packing → MUST produce different fingerprints.
    // This is the bug HEP-0034 Phase 1 corrects: pre-fix, both produced the
    // same hash, conflating layouts that occupy different memory.
    auto s_aligned =
        make_schema({make_field("flag", "bool"), make_field("val", "int32")}, "aligned");
    auto s_packed = make_schema({make_field("flag", "bool"), make_field("val", "int32")}, "packed");
    auto spec_aligned = parse_schema_json(s_aligned);
    auto spec_packed = parse_schema_json(s_packed);

    SchemaSpec empty; // no flexzone
    const auto h_aligned = compute_schema_hash(spec_aligned, empty);
    const auto h_packed = compute_schema_hash(spec_packed, empty);

    EXPECT_FALSE(h_aligned.empty());
    EXPECT_FALSE(h_packed.empty());
    EXPECT_NE(h_aligned, h_packed)
        << "aligned vs packed schemas with identical fields must have different fingerprints "
           "(HEP-CORE-0034 §6.3)";
}

TEST(SchemaValidationTest, FingerprintIncludesPacking_FlexzoneAlsoCovered)
{
    // Slot identical, flexzone packing differs → distinct fingerprints.
    auto slot = make_schema({make_field("x", "int32")}, "aligned");
    auto fz_a = make_schema({make_field("flag", "bool"), make_field("val", "int32")}, "aligned");
    auto fz_p = make_schema({make_field("flag", "bool"), make_field("val", "int32")}, "packed");
    auto slot_spec = parse_schema_json(slot);
    auto fz_aligned_spec = parse_schema_json(fz_a);
    auto fz_packed_spec = parse_schema_json(fz_p);

    const auto h_a = compute_schema_hash(slot_spec, fz_aligned_spec);
    const auto h_p = compute_schema_hash(slot_spec, fz_packed_spec);
    EXPECT_NE(h_a, h_p)
        << "flexzone packing must affect the schema fingerprint (HEP-CORE-0034 §6.3)";
}

TEST(SchemaValidationTest, FingerprintIncludesPacking_MatchingPackingProducesSameHash)
{
    // Sanity: same fields + same packing → same hash (deterministic).
    auto s1 = make_schema({make_field("x", "int32"), make_field("y", "float64")}, "aligned");
    auto s2 = make_schema({make_field("x", "int32"), make_field("y", "float64")}, "aligned");
    auto spec1 = parse_schema_json(s1);
    auto spec2 = parse_schema_json(s2);

    SchemaSpec empty;
    EXPECT_EQ(compute_schema_hash(spec1, empty), compute_schema_hash(spec2, empty));
}

// ============================================================================
// HEP-CORE-0034 Phase 4a — wire-form / SchemaSpec hash equality
// ============================================================================

TEST(SchemaValidationTest, WireForm_DatablockHalfMatchesZoneHash)
{
    // Pin (HEP-CORE-0034 §6.3): the datablock half (bytes 0..31) of the 64-byte
    // wire fingerprint equals `compute_zone_hash(slot_blds, slot_packing)`, and
    // the flexzone half (bytes 32..63) is all-zero when no flexzone is present.
    auto s = make_schema({make_field("ts", "float64"), make_field("v", "float32")}, "aligned");
    auto spec = parse_schema_json(s);

    const auto fp = compute_fingerprint_from_wire(canonical_fields_str(spec), spec.packing);
    const auto db_half = compute_zone_hash(canonical_fields_str(spec), spec.packing);
    ASSERT_EQ(fp.size(), 64u);
    EXPECT_EQ(0, std::memcmp(fp.data(), db_half.data(), 32));
    for (size_t i = 32; i < 64; ++i)
        EXPECT_EQ(fp[i], 0u) << "flexzone half must be zero when absent (byte " << i << ")";
}

TEST(SchemaValidationTest, WireForm_FlexzoneOccupiesSecondHalf)
{
    // Pin: adding a flexzone leaves the datablock half unchanged and fills the
    // flexzone half with that zone's hash; slot-only vs slot+fz fingerprints
    // differ (a producer with a flexzone must not NACK FINGERPRINT_INCONSISTENT).
    auto slot = make_schema({make_field("x", "int32")}, "aligned");
    auto fz = make_schema({make_field("y", "float64")}, "aligned");
    auto slot_spec = parse_schema_json(slot);
    auto fz_spec = parse_schema_json(fz);

    const auto fp_slot_only =
        compute_fingerprint_from_wire(canonical_fields_str(slot_spec), slot_spec.packing);
    const auto fp_with_fz =
        compute_fingerprint_from_wire(canonical_fields_str(slot_spec), slot_spec.packing,
                                      canonical_fields_str(fz_spec), fz_spec.packing);
    EXPECT_NE(fp_slot_only, fp_with_fz) << "flexzone must be part of the fingerprint";
    // Datablock half unchanged by the flexzone.
    EXPECT_EQ(0, std::memcmp(fp_slot_only.data(), fp_with_fz.data(), 32));
    // Flexzone half equals the flexzone's own zone hash.
    const auto fz_half = compute_zone_hash(canonical_fields_str(fz_spec), fz_spec.packing);
    EXPECT_EQ(0, std::memcmp(fp_with_fz.data() + 32, fz_half.data(), 32));
}

TEST(SchemaValidationTest, WireForm_ZoneAgnostic_SameLayoutSameHalf)
{
    // Pin: the same field layout hashes to the same 32-byte half whether it sits
    // in the datablock or the flexzone — position differs, half value does not.
    auto lay = make_schema({make_field("v", "float32")}, "aligned");
    auto spec = parse_schema_json(lay);
    const auto in_db = compute_fingerprint_from_wire(canonical_fields_str(spec), spec.packing);
    const auto in_fz =
        compute_fingerprint_from_wire(std::string{}, std::string{}, canonical_fields_str(spec),
                                      spec.packing);
    // db half of in_db == fz half of in_fz (same layout, same half value).
    EXPECT_EQ(0, std::memcmp(in_db.data(), in_fz.data() + 32, 32));
    EXPECT_NE(in_db, in_fz) << "same layout in different zones → different fingerprints";
}

// ============================================================================
// HEP-CORE-0034 Phase 5a — make_wire_schema_fields / apply_*_schema_fields
// ============================================================================

TEST(SchemaValidationTest, MakeWireFields_NamedSchema_PopulatesId)
{
    // Config gave a named schema as a JSON string ("$lab.x.v1"); the
    // helper should pull schema_id out of that.
    const json named_form = "$lab.demo.frame.v1";
    auto slot = make_schema({make_field("v", "float32")}, "aligned");
    auto slot_spec = parse_schema_json(slot);

    auto w = make_wire_schema_fields(named_form, slot_spec, SchemaSpec{});
    EXPECT_EQ(w.schema_id, "$lab.demo.frame.v1");
    EXPECT_FALSE(w.schema_blds.empty());
    EXPECT_EQ(w.schema_packing, "aligned");
    EXPECT_FALSE(w.schema_hash.empty());
    EXPECT_EQ(w.schema_hash.size(), 128u); // 64-byte two-zone fingerprint, hex
    EXPECT_TRUE(w.flexzone_blds.empty());
    EXPECT_TRUE(w.flexzone_packing.empty());
}

TEST(SchemaValidationTest, MakeWireFields_InlineSchema_NoId)
{
    // Inline schema (config used an object) → schema_id stays empty
    // (anonymous channel mode).
    auto slot = make_schema({make_field("v", "float32")}, "aligned");
    auto slot_spec = parse_schema_json(slot);

    auto w = make_wire_schema_fields(slot, slot_spec, SchemaSpec{});
    EXPECT_TRUE(w.schema_id.empty());
    EXPECT_FALSE(w.schema_blds.empty());
}

TEST(SchemaValidationTest, MakeWireFields_FlexzonePopulated)
{
    // Slot + flexzone both present → wire fields cover both.
    auto slot = make_schema({make_field("v", "float32")}, "aligned");
    auto fz = make_schema({make_field("p", "uint64")}, "packed");
    auto slot_spec = parse_schema_json(slot);
    auto fz_spec = parse_schema_json(fz);

    auto w = make_wire_schema_fields(nlohmann::json{}, slot_spec, fz_spec);
    EXPECT_FALSE(w.schema_blds.empty());
    EXPECT_EQ(w.schema_packing, "aligned");
    EXPECT_FALSE(w.flexzone_blds.empty());
    EXPECT_EQ(w.flexzone_packing, "packed");
    // Sanity: the hash matches what compute_fingerprint_from_wire
    // produces for the same inputs.
    const auto h_expected = compute_fingerprint_from_wire(w.schema_blds, w.schema_packing,
                                                          w.flexzone_blds, w.flexzone_packing);
    EXPECT_EQ(w.schema_hash,
              ::pylabhub::format_tools::bytes_to_hex(
                  {reinterpret_cast<const char *>(h_expected.data()), h_expected.size()}));
}

TEST(SchemaValidationTest, MakeWireFields_NoSchema_AllEmpty)
{
    // No slot, no flexzone → all empty (signals "no Stage-2 verification";
    // broker takes the legacy/anonymous path).
    auto w = make_wire_schema_fields(nlohmann::json{}, SchemaSpec{}, SchemaSpec{});
    EXPECT_TRUE(w.schema_id.empty());
    EXPECT_TRUE(w.schema_hash.empty());
    EXPECT_TRUE(w.schema_blds.empty());
    EXPECT_TRUE(w.schema_packing.empty());
    EXPECT_TRUE(w.flexzone_blds.empty());
    EXPECT_TRUE(w.flexzone_packing.empty());
}

TEST(SchemaValidationTest, ApplyProducerFields_AddsKeysWithSchemaPrefix)
{
    WireSchemaFields w;
    w.schema_id = "$lab.x.v1";
    w.schema_hash = std::string(128, 'a');
    w.schema_blds = "ts:f64:1:0";
    w.schema_packing = "aligned";
    w.flexzone_blds = "p:u64:1:0";
    w.flexzone_packing = "packed";

    json reg;
    apply_producer_schema_fields(reg, w);
    EXPECT_EQ(reg.value("schema_id", ""), "$lab.x.v1");
    EXPECT_EQ(reg.value("schema_hash", ""), w.schema_hash);
    EXPECT_EQ(reg.value("schema_blds", ""), w.schema_blds);
    EXPECT_EQ(reg.value("schema_packing", ""), "aligned");
    EXPECT_EQ(reg.value("flexzone_blds", ""), "p:u64:1:0");
    EXPECT_EQ(reg.value("flexzone_packing", ""), "packed");
    // No expected_* keys leaked into the producer payload.
    EXPECT_FALSE(reg.contains("expected_schema_id"));
    EXPECT_FALSE(reg.contains("expected_schema_blds"));
}

TEST(SchemaValidationTest, ApplyConsumerFields_AddsKeysWithExpectedPrefix)
{
    WireSchemaFields w;
    w.schema_id = "$lab.x.v1";
    w.schema_hash = std::string(128, 'a');
    w.schema_blds = "ts:f64:1:0";
    w.schema_packing = "aligned";
    w.flexzone_blds = "p:u64:1:0";
    w.flexzone_packing = "packed";

    json reg;
    apply_consumer_schema_fields(reg, w);
    EXPECT_EQ(reg.value("expected_schema_id", ""), "$lab.x.v1");
    EXPECT_EQ(reg.value("expected_schema_hash", ""), w.schema_hash);
    EXPECT_EQ(reg.value("expected_schema_blds", ""), w.schema_blds);
    EXPECT_EQ(reg.value("expected_schema_packing", ""), "aligned");
    EXPECT_EQ(reg.value("expected_flexzone_blds", ""), "p:u64:1:0");
    EXPECT_EQ(reg.value("expected_flexzone_packing", ""), "packed");
    // No producer-side keys leaked into the consumer payload.
    EXPECT_FALSE(reg.contains("schema_id"));
    EXPECT_FALSE(reg.contains("schema_blds"));
}

TEST(SchemaValidationTest, ApplyFields_EmptyWireFields_NoKeysAdded)
{
    // Empty fields → no keys added → broker takes legacy/anonymous path.
    WireSchemaFields w;
    json reg;
    apply_producer_schema_fields(reg, w);
    EXPECT_TRUE(reg.empty());

    json reg2;
    apply_consumer_schema_fields(reg2, w);
    EXPECT_TRUE(reg2.empty());
}

// ============================================================================
// HEP-CORE-0034 §6.3 — two-zone builder / comparison / verify primitives
// (compute_zone_hash absent-sentinel, make_schema_record, schema_records_
//  equivalent, verify_request_fingerprint — the split-hash comparison API)
// ============================================================================

TEST(SchemaValidationTest, ZoneHash_EmptyBldsIsAbsentSentinel)
{
    // An absent zone (empty blds) hashes to the all-zero half regardless of
    // packing — this zero is the "absent" sentinel of the 64-byte fingerprint.
    const auto z = compute_zone_hash("", "aligned");
    for (uint8_t b : z)
        EXPECT_EQ(b, 0u);
    EXPECT_EQ(compute_zone_hash("", "packed"), compute_zone_hash("", "aligned"));
    // A present zone never collides with the sentinel.
    EXPECT_NE(compute_zone_hash("v:float32:1:0", "aligned"), z);
}

TEST(SchemaValidationTest, MakeSchemaRecord_BothZones)
{
    auto rec = make_schema_record("prod.cam.uid01234567", "frame", "ts:float64:1:0", "aligned",
                                  "cal:float64:8:0", "packed");
    EXPECT_TRUE(rec.has_datablock());
    EXPECT_TRUE(rec.has_flexzone());
    EXPECT_EQ(rec.blds, "ts:float64:1:0");
    EXPECT_EQ(rec.packing, "aligned");
    EXPECT_EQ(rec.flexzone_blds, "cal:float64:8:0");
    EXPECT_EQ(rec.flexzone_packing, "packed");
    EXPECT_EQ(rec.hash, compute_fingerprint_from_wire("ts:float64:1:0", "aligned", "cal:float64:8:0",
                                                      "packed"));
}

TEST(SchemaValidationTest, MakeSchemaRecord_DatablockOnly_And_FlexzoneOnly)
{
    // Datablock-only: flexzone absent, flexzone half zero.
    auto db = make_schema_record("hub", "temp", "v:float32:1:0", "aligned");
    EXPECT_TRUE(db.has_datablock());
    EXPECT_FALSE(db.has_flexzone());
    for (size_t i = 32; i < 64; ++i)
        EXPECT_EQ(db.hash[i], 0u);

    // Flexzone-only (a shape the wire REG path can't make, but hub-globals can):
    // datablock absent, datablock half zero, flexzone half filled.
    auto fz = make_schema_record("hub", "frame_fz", "", "", "cal:float64:8:0", "aligned");
    EXPECT_FALSE(fz.has_datablock());
    EXPECT_TRUE(fz.has_flexzone());
    for (size_t i = 0; i < 32; ++i)
        EXPECT_EQ(fz.hash[i], 0u);
    EXPECT_EQ(0, std::memcmp(fz.hash.data() + 32,
                             compute_zone_hash("cal:float64:8:0", "aligned").data(), 32));
}

TEST(SchemaValidationTest, SchemaRecordsEquivalent_ByFullFingerprint)
{
    auto a = make_schema_record("o", "id", "v:float32:1:0", "aligned");
    auto same = make_schema_record("o", "id", "v:float32:1:0", "aligned");
    auto diff_packing = make_schema_record("o", "id", "v:float32:1:0", "packed");
    auto with_fz = make_schema_record("o", "id", "v:float32:1:0", "aligned", "x:int32:1:0", "aligned");
    EXPECT_TRUE(schema_records_equivalent(a, same));
    EXPECT_FALSE(schema_records_equivalent(a, diff_packing)) << "packing folds into the fingerprint";
    EXPECT_FALSE(schema_records_equivalent(a, with_fz)) << "flexzone presence changes the fingerprint";
}

TEST(SchemaValidationTest, VerifyRequestFingerprint_Consistency)
{
    const std::string db = "ts:float64:1:0";
    const std::string fz = "cal:float64:8:0";
    const auto fp = compute_fingerprint_from_wire(db, "aligned", fz, "packed");
    const std::string claimed = ::pylabhub::format_tools::bytes_to_hex(
        {reinterpret_cast<const char *>(fp.data()), fp.size()});

    // Correct 128-hex claim → consistent, recomputed 64-byte fingerprint matches.
    auto ok = verify_request_fingerprint(db, "aligned", fz, "packed", claimed);
    EXPECT_TRUE(ok.consistent);
    EXPECT_EQ(ok.hash, fp);
    // Flip the flexzone packing → the claim no longer matches → inconsistent.
    auto bad = verify_request_fingerprint(db, "aligned", fz, "aligned", claimed);
    EXPECT_FALSE(bad.consistent);
    // Empty claim → "just compute": consistent, hash still the full fingerprint.
    auto compute_only = verify_request_fingerprint(db, "aligned", fz, "packed", "");
    EXPECT_TRUE(compute_only.consistent);
    EXPECT_EQ(compute_only.hash, fp);
}
