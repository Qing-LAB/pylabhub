# HEP-CORE-0016: Named Schema Registry

| Property      | Value                                                                   |
|---------------|-------------------------------------------------------------------------|
| **HEP**       | `HEP-CORE-0016`                                                         |
| **Title**     | Named Schema Registry — Channel Schema Identity and Discovery            |
| **Status**    | Design — not yet implemented                                            |
| **Created**   | 2026-03-01                                                              |
| **Area**      | Schema System (`pylabhub-utils`, broker, actor, processor)              |
| **Depends on**| HEP-CORE-0002 (DataHub), HEP-CORE-0007 (Protocol), HEP-CORE-0012 (Processor) |

---

## 1. Motivation

Channel schemas (slot layout + flexzone layout) are currently defined inline inside
every actor and processor config file that touches the channel:

```json
// actor_A.json
"slot_schema": { "fields": [{"name": "ts", "type": "float64"}, {"name": "value", "type": "float32"}] }

// processor.json — same channel, same layout, repeated
"in_slot_schema": { "fields": [{"name": "ts", "type": "float64"}, {"name": "value", "type": "float32"}] }
```

This creates several problems:

| Problem | Impact |
|---------|--------|
| No single source of truth | Schema changes require touching every config that references the channel |
| No discoverability | "What does channel X carry?" requires parsing every actor/processor config |
| No schema identity | Schema hash is the only identifier; humans have no name to reason about |
| No version tracking | Breaking changes are invisible until hash mismatches appear at runtime |
| C++ unnamed only | C++ template structs have no linkage to any human-readable schema definition |

This HEP defines a **Named Schema Registry** that gives schemas a stable identity,
makes them discoverable by name, and lets the broker annotate unnamed schemas
against known named ones — all without breaking existing unnamed schema support.

---

## 2. Core Principle

> **The checksum is the primary identity. The name is a human-readable alias for a
> known checksum.**

The BLAKE2b-256 hash of the BLDS string (already computed for every DataBlock channel,
per HEP-CORE-0002 §11) uniquely identifies a schema's binary layout. A name adds
discoverability and documentation on top — it does not replace the hash.

Consequences:

- A C++ producer using a compile-time struct (unnamed) and a Python processor using a
  named schema are structurally compatible when their hashes agree.
- The version number is implicit in the hash: you cannot claim `temperature.raw@2`
  with a `@1` hash.
- Naming is opt-in. Unnamed schemas remain fully valid and fully supported.
- The broker can annotate an unnamed schema as a named one by reverse hash lookup.

---

## 3. Terminology

| Term | Definition |
|------|-----------|
| **Named schema** | Schema with a registered ID (`lab.sensors.temperature.raw@1`); defined in the schema library; broker can look it up by name |
| **Unnamed schema** | Schema with no registered ID; defined inline in config or by C++ struct; identified only by hash |
| **Schema library** | File-based store of named schema JSON files; no lifecycle dependency |
| **Schema registry** | Lifecycle module wrapping the library; adds file watching, broker query integration, and singleton cache |
| **Annotation** | Broker reverse-lookup: unnamed schema hash → matching named schema ID |
| **BLDS** | Basic Layout Description String — existing canonical text representation of a schema layout (HEP-CORE-0002 §11) |

---

## 4. Schema ID Format

```
{namespace}.{name}@{version}
```

- **Namespace**: dot-separated hierarchical path, lowercase, no spaces
  (`lab.sensors`, `io.camera`, `control.motor`)
- **Name**: last segment before `@`, describes the data shape
  (`temperature.raw`, `frame.rgb`, `setpoint`)
- **Version**: positive integer; incremented on any breaking change
  (field added, removed, renamed, or type changed)
- **`@latest`**: reserved for tooling only; never written in production configs

Examples:
```
lab.sensors.temperature.raw@1
lab.sensors.pressure.differential@2
io.camera.frame.rgb@1
control.motor.setpoint@1
```

---

## 5. Directory Structure

The schema library maps namespace hierarchy to a filesystem tree.

```
<schema_root>/
  lab/
    sensors/
      temperature.raw.v1.json
      temperature.raw.v2.json       ← breaking change: field added
      temperature.processed.v1.json
    pressure/
      differential.v1.json
    alerts/
      threshold_breach.v1.json
  io/
    camera/
      frame.rgb.v1.json
      frame.depth.v1.json
  control/
    motor/
      setpoint.v1.json
```

**ID ↔ path mapping:**

| Schema ID | File path |
|-----------|-----------|
| `lab.sensors.temperature.raw@1` | `lab/sensors/temperature.raw.v1.json` |
| `io.camera.frame.rgb@1` | `io/camera/frame.rgb.v1.json` |

Rule: dots become path separators; `@N` suffix becomes `.vN` filename suffix.

**Search path (priority: first wins):**

1. `PYLABHUB_SCHEMA_PATH` env var (colon-separated directories)
2. `<hub_dir>/schemas/` — per-hub, per-deployment
3. `~/.pylabhub/schemas/` — user-global
4. `/usr/share/pylabhub/schemas/` — system-wide

---

## 6. Schema JSON Format

```json
{
  "id":      "lab.sensors.temperature.raw",
  "version": 1,
  "description": "Raw temperature measurement — ADC values + timestamp",

  "slot": {
    "packing": "natural",
    "fields": [
      {"name": "ts",        "type": "float64", "unit": "s",
       "description": "Unix epoch (seconds)"},
      {"name": "samples",   "type": "float32", "count": 8,
       "description": "ADC channel values"},
      {"name": "sensor_id", "type": "uint16",
       "description": "Hardware sensor index"},
      {"name": "_pad",      "type": "uint16",
       "description": "Alignment padding"}
    ]
  },

  "flexzone": {
    "packing": "natural",
    "fields": [
      {"name": "cal_factors", "type": "float64", "count": 8},
      {"name": "sensor_uid",  "type": "uint64"}
    ]
  },

  "metadata": {
    "author":  "lab-instruments-team",
    "created": "2026-03-01",
    "tags":    ["temperature", "raw", "adc"]
  }
}
```

### 6.1 Field Type System

Directly mirrors the existing BLDS type system (`schema_blds.hpp`). No new type
identifiers are introduced.

| JSON `"type"` | BLDS token | C++ type | Size |
|---------------|-----------|----------|------|
| `"float32"` | `f32` | `float` | 4 |
| `"float64"` | `f64` | `double` | 8 |
| `"int8"` | `i8` | `int8_t` | 1 |
| `"int16"` | `i16` | `int16_t` | 2 |
| `"int32"` | `i32` | `int32_t` | 4 |
| `"int64"` | `i64` | `int64_t` | 8 |
| `"uint8"` | `u8` | `uint8_t` | 1 |
| `"uint16"` | `u16` | `uint16_t` | 2 |
| `"uint32"` | `u32` | `uint32_t` | 4 |
| `"uint64"` | `u64` | `uint64_t` | 8 |
| `"bool"` | `b` | `bool` | 1 |
| `"char"` | `c` | `char` | 1 |

**Arrays**: any primitive type can carry `"count": N` (N ≥ 2). This includes all
numeric types — not just `char`/`byte`:

```json
{"name": "samples",   "type": "float32", "count": 1024}
{"name": "label",     "type": "char",    "count": 32  }
{"name": "histogram", "type": "uint32",  "count": 64  }
```

Maps to BLDS: `f32[1024]`, `c[32]`, `u32[64]`. Absent or `"count": 1` = scalar.

**Not permitted in schema JSON**: nested structs, pointers, variable-length fields,
unions. Schemas describe a fixed-size flat layout only.

### 6.2 Alignment

`"packing": "natural"` is the only supported value (and the default). Offsets and
total size are **computed** by the library from field order and natural alignment
rules — they are not declared in the JSON. This prevents the file from lying about
padding and keeps it consistent with how C++ naturally lays out structs.

The library computes each field's byte offset using the same rules as the C++ compiler
under natural/platform alignment, then derives:

```
slot_size     = sum of field byte sizes + compiler-inserted padding
flexzone_size = same
slot_blds     = BLDS/1.0 string over slot fields (name:type pairs, no explicit offsets
                for user schemas — offsets are implicit in field order)
slot_hash     = BLAKE2b-256(slot_blds)
flexzone_hash = BLAKE2b-256(flexzone_blds)
```

The BLDS for user schemas omits `@offset:size` (used only for `SharedMemoryHeader`
ABI verification per HEP-CORE-0002). This is sufficient for schema identity because
natural alignment is deterministic given field names and types.

---

## 7. Named vs Unnamed — Coexistence

Both are valid. They converge at the same wire representation.

| | Named schema | Unnamed schema |
|--|--------------|----------------|
| `schema_id` in config | `"in_schema": "lab.sensors.temperature.raw@1"` | `"in_slot_schema": { "fields": [...] }` |
| `schema_id` on wire (broker) | `"lab.sensors.temperature.raw@1"` | `""` (empty) |
| `schema_hash` on wire | BLAKE2b of BLDS | BLAKE2b of BLDS |
| Broker validation | Hash must match registry entry for that ID | Hash consistency enforced across channel |
| Broker annotation | Already named | Broker attempts reverse hash → ID lookup |
| C++ `ProducerOptions` | `opts.schema_id = "lab.sensors.temperature.raw@1"` | `opts.schema_id` not set |
| Discoverability | `SCHEMA_REQ` → ID + BLDS | `SCHEMA_REQ` → `""` + BLDS |

**Backward compatibility**: all existing configs using inline `slot_schema` / `flexzone_schema`
continue to work unchanged. The inline form is the unnamed path.

---

## 8. Broker Protocol

### 8.1 REG_REQ Extension

`REG_REQ` (producer registration) gains one new optional field:

```
schema_id  : string  (empty = unnamed)
```

The existing `schema_hash` and BLDS fields are unchanged.

### 8.2 Broker Annotation Logic

On receiving `REG_REQ`:

```
Case A — schema_id provided:
  1. Look up schema_id in registry → expected_hash
  2. If hash matches   → confirmed named schema; store (channel, schema_id, hash, blds)
  3. If hash mismatch  → REG_NACK with reason:
       "schema_id 'lab.sensors.temperature.raw@1' declared but hash does not match
        registry — struct layout differs from named schema definition"

Case B — schema_id empty (unnamed):
  1. Look up hash in registry bidirectional map → matching_id (may be empty)
  2. If found  → annotate: channel carries matching_id (log at INFO level)
               → store (channel, matching_id, hash, blds)
  3. If not found → store (channel, "", hash, blds)  — anonymous, fully valid
```

### 8.3 SCHEMA_REQ (new broker message)

Any participant can query the broker for a channel's schema after `DISC_ACK`:

```
Request:   SCHEMA_REQ  { channel_name: string }
Response:  SCHEMA_ACK  { channel_name: string,
                         schema_id:   string,   // "" if anonymous
                         blds:        string,   // always present
                         hash:        bytes[32] }
```

This allows consumers and processors to reconstruct the ctypes struct definition
at runtime without needing a local schema file — particularly useful when the channel
carries an unnamed schema or when the receiver is on a different machine.

### 8.4 Consumer and Processor Registration

`CONSUMER_REG_REQ` gains the same optional `schema_id` field. The broker cross-checks
the expected hash against the channel's stored hash:

```
If expected_hash != channel_hash → CONSUMER_REG_NACK with schema mismatch detail
```

This is the same check already performed via `expected_schema_hash` — the extension
adds the human-readable `schema_id` to the error message.

---

## 9. C++ Integration

### 9.1 ProducerOptions / ConsumerOptions

```cpp
struct ProducerOptions {
    // ... existing fields ...

    /// Optional: named schema ID for validation at create() time.
    /// If set and SchemaRegistry is reachable, the registry verifies:
    ///   sizeof(F) == schema.slot_size
    ///   sizeof(D) == schema.flexzone_size
    ///   SchemaTraits<F>::hash == schema.slot_hash
    /// Mismatch → SchemaValidationException (hard error).
    std::string schema_id;  // "" = unnamed, no registry check
};
```

`ConsumerOptions` gains `expected_schema_id` with the same semantics.

### 9.2 Compile-time Check (opt-in)

Existing `PYLABHUB_SCHEMA_BEGIN/MEMBER/END` macros generate `SchemaTraits<T>`
specializations. The `create<F,D>()` factory uses these if available:

```cpp
// Struct with registered traits
PYLABHUB_SCHEMA_BEGIN(RawTempSlot)
  PYLABHUB_SCHEMA_MEMBER(ts)
  PYLABHUB_SCHEMA_MEMBER(samples)
  PYLABHUB_SCHEMA_MEMBER(sensor_id)
  PYLABHUB_SCHEMA_MEMBER(_pad)
PYLABHUB_SCHEMA_END(RawTempSlot)

// Named check at create() time (registry searched via default search path)
ProducerOptions opts;
opts.schema_id = "lab.sensors.temperature.raw@1";
auto prod = hub::Producer::create<RawTempSlot, CalibFZ>(messenger, opts);
// → registry loaded → sizeof + hash verified → SchemaValidationException on mismatch
```

Without `opts.schema_id`: no registry lookup, no change from today.

### 9.3 Unnamed C++ Path (unchanged)

```cpp
// No schema_id — unnamed. No registry involved. Same as today.
auto prod = hub::Producer::create<RawTempSlot, CalibFZ>(messenger, opts);
```

The broker may still annotate the channel if the hash matches a named schema.

---

## 10. Script / Python Integration

### 10.1 Named schema in processor.json / actor.json

```json
{
  "in_schema":  "lab.sensors.temperature.raw@1",
  "out_schema": "lab.sensors.temperature.processed@1",
  "schema_dir": "./schemas"
}
```

`schema_dir` overrides the default search path for this component.

Inline (`in_slot_schema` field list) is still accepted and takes priority if both
are present (for migration compatibility).

### 10.2 Runtime ctypes generation

The script host resolves the schema (library or SCHEMA_REQ to broker), then calls:

```python
schema.to_ctypes_struct("RawTempSlot")
# → generates and injects Python ctypes class into the script's namespace
# → works for both named and unnamed schemas (broker always has the BLDS)
```

---

## 11. SchemaLibrary and SchemaRegistry

### 11.1 SchemaLibrary (no lifecycle)

Plain utility class. Usable by standalone tools (`pylabhub-processor`, CLI
schema inspector) without a lifecycle stack.

```cpp
namespace pylabhub::schema {

class SchemaLibrary {
public:
    explicit SchemaLibrary(std::vector<std::string> search_dirs);
    static SchemaLibrary default_search_path(const std::string& hub_dir = {});

    // Forward: id → SchemaDef
    std::optional<SchemaDef> get(const std::string& id) const;

    // Reverse: hash → id  (annotation path)
    std::optional<std::string> identify(const std::array<uint8_t,32>& hash) const;

    std::vector<std::string> list() const;
    void register_schema(const SchemaDef& def);  // in-memory, for tests

private:
    std::vector<std::string> search_dirs_;
    mutable std::mutex       cache_mu_;
    mutable std::unordered_map<std::string, SchemaDef>             id_cache_;
    mutable std::unordered_map<std::string, std::string>           hash_to_id_;
};

} // namespace pylabhub::schema
```

### 11.2 SchemaRegistry (lifecycle module)

Singleton lifecycle module for long-running services (hub, actor). Wraps
`SchemaLibrary` and adds:

- File watching (inotify / `ReadDirectoryChangesW`) — invalidates cache on change,
  warns if updated file's hash conflicts with broker's live channel
- Broker query integration — falls back to `SCHEMA_REQ` when schema not in local files
- Mismatch warning: "file says @2, broker says @1 — producer not yet restarted"

```cpp
class SchemaRegistry {
public:
    static ModuleDef        GetLifecycleModule();  // deps: Logger, (optional) Messenger
    static SchemaRegistry&  get_instance();

    std::optional<SchemaDef> get(const std::string& id) const;
    std::optional<SchemaDef> get_for_channel(const std::string& channel_name) const;
    std::optional<std::string> identify(const std::array<uint8_t,32>& hash) const;

    void publish(const std::string& channel_name, const SchemaDef& def);
};
```

---

## 12. Module Boundaries

| Module | Lifecycle | Concern |
|--------|-----------|---------|
| `schema_def.hpp` | none | `SchemaDef`, `FieldDef`, `FieldType` — pure data |
| `schema_parser.cpp` | none | JSON ↔ `SchemaDef`, BLDS generation, hash computation |
| `schema_library.hpp/cpp` | none | File search, forward/reverse lookup, in-memory cache |
| `schema_registry.hpp/cpp` | yes (Logger) | Singleton, file watcher, broker query fallback |

`JsonConfig` is **not** used for schema loading. Schema files are immutable definition
files loaded on demand; `JsonConfig` is for mutable application config with env var
overrides and lifecycle-managed reloading. A simple `nlohmann::json` parse suffices.

---

## 13. Phased Delivery

### Phase 1 — Schema Library + JSON format

- `SchemaDef`, `FieldDef`, `FieldType`, `SchemaLibrary`
- JSON parser: reads schema files, computes BLDS + hash, validates field types
- `SchemaLibrary::identify()` — reverse hash lookup
- `SchemaDef::to_datablock_config()`, `to_ctypes_struct()`
- Unit tests: load, round-trip, hash stability, array types, natural alignment

### Phase 2 — C++ Integration

- `ProducerOptions::schema_id` / `ConsumerOptions::expected_schema_id`
- `create<F,D>()` size + hash check when schema_id is set
- `PYLABHUB_SCHEMA_TRAITS` macro (convenience wrapper around existing `PYLABHUB_SCHEMA_BEGIN/MEMBER/END`)
- Tests: mismatch detection, unnamed pass-through, named validation

### Phase 3 — Broker Protocol + Annotation

- `REG_REQ` / `CONSUMER_REG_REQ`: add `schema_id` field
- Broker annotation logic (Case A / Case B, §8.2)
- `SCHEMA_REQ` / `SCHEMA_ACK` new message pair
- Mismatch → `REG_NACK` with human-readable reason including schema_id and version

### Phase 4 — SchemaRegistry Lifecycle Module

- File watcher (inotify / `ReadDirectoryChangesW`)
- Broker query fallback via `SCHEMA_REQ`
- Mismatch warning when file version ≠ broker's live channel version

### Phase 5 — Script Integration

- `processor.json` / `actor.json`: `in_schema` / `out_schema` fields
- Script host: resolve schema → generate ctypes → inject into Python namespace
- Fallback: `SCHEMA_REQ` to broker when no local file (cross-machine processor)

---

## 14. Open Questions

| # | Question | Status |
|---|----------|--------|
| 1 | Should `SCHEMA_REQ` be a separate ZMQ socket or ride on the existing broker REQ/REP? | Open |
| 2 | Schema inheritance / composition (`"extends": "base.schema@1`)? | Deferred to Phase 6 |
| 3 | Schema migration tooling (`diff`, `validate`, `migrate`)? | Out of scope for this HEP |
| 4 | Should the broker persist its schema store to disk across restarts? | Open |
| 5 | Maximum schema file size / field count limits? | TBD during Phase 1 implementation |
