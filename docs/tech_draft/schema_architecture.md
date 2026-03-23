# Tech Draft: Schema Architecture

**Status**: Reference (2026-03-23)
**Branch**: `feature/lua-role-support`

## 1. Overview

pyLabHub has a layered schema system that describes data slot structure at
compile time, in configuration files, in script engines, and on the wire.
Each layer has its own type family — no duplication, clear conversion points.

```
JSON schema file (.json)
    |
    v
SchemaLibrary (L1: Named Registry)
    |
    +---> SchemaEntry {SchemaInfo, SchemaLayoutDef}
    |         |
    |         +---> [C++ compile-time] PYLABHUB_SCHEMA macros -> SchemaInfo (L0)
    |         |
    |         +---> [Python/Lua runtime] schema_entry_to_spec() -> SchemaSpec (L2)
    |         |
    |         +---> [ZMQ transport] schema_spec_to_zmq_fields() -> ZmqSchemaField[] (L3)
    |
    +---> InboxConfig.schema_json (L4: Config)
    |
    +---> ChannelSchemaInfo (L5: Broker Protocol)
```

## 2. Layer Definitions

### L0: Compile-Time (BLDS)

**File**: `src/include/utils/schema_blds.hpp`
**Namespace**: `pylabhub::schema`

Types:
- `SchemaVersion` — semantic version packed into uint32_t (10.10.12 bits)
- `SchemaInfo` — complete compile-time schema: BLDS string, BLAKE2b hash, struct_size
- `BLDSTypeID<T>` — template mapping C++ types to BLDS type IDs (f32, i64, u8, c[N], etc.)
- `BLDSBuilder` — manual BLDS string builder for custom layouts
- `SchemaRegistry<T>` — compile-time specialization via `PYLABHUB_SCHEMA_BEGIN/MEMBER/END` macros

Purpose: Introspect C++ structs at compile time, generate canonical BLDS strings,
compute BLAKE2b hashes for schema identity. Used by the template factory functions
(`create_datablock_producer<FlexZoneT, DataBlockT>()`) to validate schema match.

12 fundamental types: b, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, c.
Arrays: `c[N]`, `f32[4]`, etc. Compound structs: `_BLAKE2BHEX`.

### L1: Named Schema Registry

**Files**: `src/include/utils/schema_def.hpp`, `schema_library.hpp`, `schema_registry.hpp`
**Namespace**: `pylabhub::schema`

Types:
- `SchemaFieldDef` — one field from JSON schema (name, type, count, unit, description)
- `SchemaLayoutDef` — container for a layout section's fields (slot or flexzone)
- `SchemaEntry` — complete named schema: ID, version, slot+flexzone layouts, computed SchemaInfo
- `SchemaLibrary` — plain utility: in-memory store with forward (ID->entry) and reverse (hash->ID) lookup
- `SchemaStore` — lifecycle-managed singleton wrapping SchemaLibrary with mutex (HEP-0016)

Purpose: Load schema definitions from JSON files, directory scanning, bidirectional
lookup by ID or hash. SchemaEntry contains both the parsed layout and the computed
SchemaInfo (BLDS + hash) for each section.

JSON types: float32/64, int8/16/32/64, uint8/16/32/64, bool, char. Count for arrays.

### L2: Scripting / Runtime

**File**: `src/include/utils/script_host_schema.hpp`
**Namespace**: `pylabhub::scripting`

Types:
- `FieldDef` — one typed field for ctypes struct building (name, type_str, count, byte_length)
- `SlotExposure` — enum: Ctypes (named fields) or NumpyArray (dtype + shape)
- `SchemaSpec` — complete schema for one buffer: fields[], packing, exposure, numpy info

Purpose: Runtime representation consumed by Python (ctypes.Structure) and Lua (FFI cdef).
Built from JSON config via `parse_schema_json()` or from registry via `schema_entry_to_spec()`.
Stored in `RoleHostCore::fz_spec_` for flexzone.

13-type set: bool, int8/16/32/64, uint8/16/32/64, float32/64, string, bytes.

### L3: Wire / Transport

**File**: `src/include/utils/hub_zmq_queue.hpp`
**Namespace**: `pylabhub::hub`

Types:
- `ZmqSchemaField` — one typed field for ZMQ frame encoding (type_str, count, byte_length)

Purpose: Describes the msgpack wire format for ZMQ PUSH/PULL transport.
Wire frame: `fixarray[4] = [magic, schema_tag:bin8, seq:uint64, payload:array(N)]`.
Factory validates: non-empty schema, depth>0, valid type_str, string/bytes length>0.

Same 13-type set as FieldDef — intentional parity ensures ctypes and ZMQ are compatible.

### L4: Configuration

**File**: `src/include/utils/config/inbox_config.hpp`
**Namespace**: `pylabhub::config`

Types:
- `InboxConfig` — contains `schema_json` (nlohmann::json), `endpoint`, `buffer_depth`, `overflow_policy`

Purpose: Stores the raw JSON schema object from config. Parsed lazily — converted to
SchemaSpec or ZmqSchemaField[] only when the inbox is actually set up.

### L5: Broker Protocol

**File**: `src/include/utils/messenger.hpp`
**Namespace**: `pylabhub::hub`

Types:
- `ChannelSchemaInfo` — schema info returned by `query_channel_schema()`: schema_id, blds, hash_hex
- `RoleInfoResult` — role inbox info: inbox_endpoint, inbox_schema[], inbox_packing

Purpose: Query/response messages for schema discovery. Broker stores schema info
from REG_REQ and returns it via SCHEMA_REQ/ROLE_INFO_REQ.

## 3. Conversion Points

| From | To | Function | Location |
|------|----|----------|----------|
| SchemaLayoutDef (L1) | SchemaSpec (L2) | `schema_entry_to_spec()` | `script_host_helpers.hpp` |
| JSON object (L4) | SchemaSpec (L2) | `parse_schema_json()` | `script_host_helpers.hpp` |
| SchemaSpec (L2) | ZmqSchemaField[] (L3) | `schema_spec_to_zmq_fields()` | `script_host_helpers.hpp` |
| SchemaSpec (L2) | BLAKE2b hash | `compute_schema_hash()` | `script_host_helpers.hpp` |

All conversion functions are in `script_host_helpers.hpp` (private to scripting layer).

## 4. Type Set Agreement

All runtime layers (L2, L3) use the same 13 scalar/array types:

| Type string | C equivalent | Size (bytes) |
|-------------|-------------|--------------|
| `bool` | `bool` | 1 |
| `int8` | `int8_t` | 1 |
| `int16` | `int16_t` | 2 |
| `int32` | `int32_t` | 4 |
| `int64` | `int64_t` | 8 |
| `uint8` | `uint8_t` | 1 |
| `uint16` | `uint16_t` | 2 |
| `uint32` | `uint32_t` | 4 |
| `uint64` | `uint64_t` | 8 |
| `float32` | `float` | 4 |
| `float64` | `double` | 8 |
| `string` | `char[N]` | N (config) |
| `bytes` | `uint8_t[N]` | N (config) |

The compile-time layer (L0) uses BLDS codes (b, i8, f32, c, etc.) which map 1:1.

## 5. Design Principles

1. **No duplication** — each type has one definition in one file
2. **Layer isolation** — compile-time types don't depend on runtime; wire types don't depend on registry
3. **Single conversion path** — registry->scripting->wire, all in `script_host_helpers.hpp`
4. **Type parity** — FieldDef and ZmqSchemaField share the same 13-type vocabulary by design
5. **Lazy evaluation** — config stores raw JSON; conversion happens only when needed
