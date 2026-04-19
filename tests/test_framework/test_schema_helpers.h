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

/// Exercises every scalar type the ScriptEngine implementations support
/// (Lua, Python, Native) — see src/scripting/lua_engine.cpp:623-635 and
/// src/scripting/python_helpers.hpp:119-127 for the dispatcher tables
/// this schema exists to cover.
///
/// Fields are ordered in descending alignment so no *internal* padding
/// between fields is needed under the aligned packing rules — any
/// alignment mismatch between the engine's ffi/ctypes type and the raw
/// buffer shows up as a value read from the wrong offset.
///
/// The struct's overall alignment is 8 bytes (the float64/int64/uint64
/// requirement), so the aligned layout does include 1 byte of trailing
/// padding: 8+8+8+4+4+4+2+2+1+1+1+4+8 = 55 bytes of field data, rounded
/// up to 56 (next multiple of 8). Packed drops that trailing byte → 55.
/// This difference is what lets a paired aligned-vs-packed test verify
/// the packing argument is actually honoured (see e.g.
/// register_slot_type_all_supported_types in lua_engine_workers.cpp).
///
/// Types covered:
///   bool, int8/16/32/64, uint8/16/32/64, float32, float64, bytes, string
/// (13 types, one field each). uint64 is the type that was missing from
/// the prior helpers and is the least likely to get accidental coverage
/// from the float/int32 paths.
///
/// Always use `compute_schema_size` from `schema_utils.hpp` to get the
/// actual size rather than relying on a hardcoded constant — the
/// aligned layout depends on platform rules the helper already encodes
/// correctly.
inline SchemaSpec all_types_schema()
{
    SchemaSpec spec;
    spec.has_schema = true;
    // 8-byte-aligned first (no padding needed at start)
    spec.fields.push_back({"f64",    "float64", 1, 0});
    spec.fields.push_back({"i64",    "int64",   1, 0});
    spec.fields.push_back({"u64",    "uint64",  1, 0});
    // 4-byte-aligned
    spec.fields.push_back({"f32",    "float32", 1, 0});
    spec.fields.push_back({"i32",    "int32",   1, 0});
    spec.fields.push_back({"u32",    "uint32",  1, 0});
    // 2-byte-aligned
    spec.fields.push_back({"i16",    "int16",   1, 0});
    spec.fields.push_back({"u16",    "uint16",  1, 0});
    // 1-byte-aligned
    spec.fields.push_back({"b",      "bool",    1, 0});
    spec.fields.push_back({"i8",     "int8",    1, 0});
    spec.fields.push_back({"u8",     "uint8",   1, 0});
    spec.fields.push_back({"bytes4", "bytes",   1, 4});
    spec.fields.push_back({"str8",   "string",  1, 8});
    return spec;
}

} // namespace pylabhub::tests
