/**
 * @file test_schema_validation.cpp
 * @brief Tests for schema parsing, validation, and size computation.
 *
 * Covers parse_schema_json() error paths, compute_field_layout() edge cases,
 * and compute_schema_size() correctness for all field types.
 */
#include "utils/schema_utils.hpp"
#include "utils/schema_field_layout.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::hub;
using json = nlohmann::json;

// ============================================================================
// Helpers
// ============================================================================

static json make_field(const std::string &name, const std::string &type,
                       uint32_t count = 1, uint32_t length = 0)
{
    json f;
    f["name"] = name;
    f["type"] = type;
    if (count != 1)  f["count"]  = count;
    if (length != 0) f["length"] = length;
    return f;
}

static json make_schema(std::initializer_list<json> fields,
                        const std::string &packing = "aligned")
{
    json s;
    s["fields"]  = json::array();
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
// ============================================================================

TEST(SchemaValidationTest, ParseError_EmptyFieldsArray)
{
    json s;
    s["fields"] = json::array();
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

TEST(SchemaValidationTest, ParseError_MissingFieldsKey)
{
    json s;
    s["packing"] = "aligned";
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

TEST(SchemaValidationTest, ParseError_MissingFieldName)
{
    json s;
    s["fields"] = json::array();
    json f;
    f["type"] = "int32";
    // No "name" key
    s["fields"].push_back(f);
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

TEST(SchemaValidationTest, ParseError_MissingFieldType)
{
    json s;
    s["fields"] = json::array();
    json f;
    f["name"] = "x";
    // No "type" key
    s["fields"].push_back(f);
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

TEST(SchemaValidationTest, ParseError_InvalidTypeStr)
{
    auto s = make_schema({make_field("x", "complex128")});
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

TEST(SchemaValidationTest, ParseError_CountZero)
{
    json s = make_schema({make_field("x", "int32")});
    s["fields"][0]["count"] = 0;
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

TEST(SchemaValidationTest, ParseError_StringLengthZero)
{
    auto s = make_schema({make_field("label", "string", 1, 0)});
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

TEST(SchemaValidationTest, ParseError_BytesLengthZero)
{
    auto s = make_schema({make_field("data", "bytes", 1, 0)});
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

TEST(SchemaValidationTest, ParseError_ExposeAsPresent)
{
    json s;
    s["expose_as"] = "SomeType";
    s["fields"] = json::array({make_field("x", "int32")});
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

TEST(SchemaValidationTest, ParseError_InvalidPacking)
{
    auto s = make_schema({make_field("x", "int32")}, "natural");
    EXPECT_THROW(parse_schema_json(s), std::runtime_error);
}

// ============================================================================
// compute_schema_size — all 13 field types
// ============================================================================

TEST(SchemaValidationTest, SchemaSize_AllNumericTypes)
{
    // Each numeric type should produce a non-zero size.
    const char *types[] = {"bool", "int8", "uint8", "int16", "uint16",
                           "int32", "uint32", "int64", "uint64",
                           "float32", "float64"};
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
        {"ts", "float64", 1, 0},
        {"data", "float32", 4, 0},
        {"label", "string", 1, 32}
    };
    auto descs = to_field_descs(fields);
    ASSERT_EQ(descs.size(), 3u);
    EXPECT_EQ(descs[0].type_str, "float64");
    EXPECT_EQ(descs[0].count, 1u);
    EXPECT_EQ(descs[1].type_str, "float32");
    EXPECT_EQ(descs[1].count, 4u);
    EXPECT_EQ(descs[2].type_str, "string");
    EXPECT_EQ(descs[2].length, 32u);
}
