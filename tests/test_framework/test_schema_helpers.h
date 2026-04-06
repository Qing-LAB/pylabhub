#pragma once
/**
 * @file test_schema_helpers.h
 * @brief Shared schema definitions for engine tests.
 *
 * Common SchemaSpec factories used across Python, Lua, and Native engine tests.
 * Avoids copy-pasting identical schema definitions in each test file.
 */

#include "utils/schema_types.hpp"

#include <string>

namespace pylabhub::tests
{

using hub::SchemaSpec;
using hub::FieldDef;

/// Single float32 field — minimal schema for basic tests.
inline SchemaSpec simple_schema()
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back({"value", "float32", 1, 0});
    return spec;
}

/// float64 + uint8 + int32 — padding-sensitive (16 bytes aligned, 13 packed).
inline SchemaSpec padding_schema()
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back({"ts",    "float64", 1, 0});
    spec.fields.push_back({"flag",  "uint8",   1, 0});
    spec.fields.push_back({"count", "int32",   1, 0});
    return spec;
}

/// 6-field schema: float64 + float32[3] + uint16 + bytes[5] + string[16] + int64
/// Exercises all alignment paths (56 bytes aligned, 51 packed).
inline SchemaSpec complex_mixed_schema()
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back({"timestamp", "float64", 1, 0});
    spec.fields.push_back({"data",      "float32", 3, 0});
    spec.fields.push_back({"status",    "uint16",  1, 0});
    spec.fields.push_back({"raw",       "bytes",   1, 5});
    spec.fields.push_back({"label",     "string",  1, 16});
    spec.fields.push_back({"seq",       "int64",   1, 0});
    return spec;
}

/// Flexzone schema: uint32 + float64[2] — padding between scalar and array (24 bytes aligned).
inline SchemaSpec fz_array_schema()
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back({"version", "uint32",  1, 0});
    spec.fields.push_back({"values",  "float64", 2, 0});
    return spec;
}

/// 5-field schema matching the native multifield test module:
/// float64 + uint8 + int32 + float32[3] + bytes[8] = 40 bytes aligned.
inline SchemaSpec multifield_schema()
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back({"ts",     "float64", 1, 0});
    spec.fields.push_back({"flag",   "uint8",   1, 0});
    spec.fields.push_back({"count",  "int32",   1, 0});
    spec.fields.push_back({"values", "float32", 3, 0});
    spec.fields.push_back({"tag",    "bytes",   1, 8});
    return spec;
}

} // namespace pylabhub::tests
