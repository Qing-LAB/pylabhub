# Design Verification Checklist

**Purpose:** Confirm design and API claims against actual code. Every checked item has a code reference.  
**Rule:** See `DESIGN_VERIFICATION_RULE.md` – no claim is "done" without verification.

**Last verification pass:** 2026-02-15 (codebase as of this date).

---

## 1. Producer creation

### 1.1 Template `create_datablock_producer<FlexZoneT, DataBlockT>(..., config)`

- [x] **Schema generation** – Both schemas are generated from template types at compile time.
  - Code: `data_block.hpp` lines 1416–1419:
    - `generate_schema_info<FlexZoneT>("FlexZone", ...)`
    - `generate_schema_info<DataBlockT>("DataBlock", ...)`
- [x] **Schemas passed to impl** – Both schema pointers are passed (never null from this path).
  - Code: `data_block.hpp` lines 1441–1442: `create_datablock_producer_impl(..., &flexzone_schema, &datablock_schema)`.
- [x] **Impl stores both hashes when non-null** – Header receives both hashes.
  - Code: `data_block.cpp` `create_datablock_producer_impl()`:
    - Lines 3069–3076: if `flexzone_schema != nullptr` → `memcpy(header->flexzone_schema_hash, ...)`.
    - Lines 3082–3090: if `datablock_schema != nullptr` → `memcpy(header->datablock_schema_hash, ...)`, `header->schema_version = ...`.
- [x] **Size checks before create** – FlexZone and DataBlock sizes validated against config.
  - Code: `data_block.hpp` lines 1422–1438: `config.flex_zone_size < sizeof(FlexZoneT)` and `slot_size < sizeof(DataBlockT)` throw `std::invalid_argument`.

### 1.2 Non-template `create_datablock_producer(hub, name, policy, config)`

- [x] **C++ API, not C API** – These are C++ functions (e.g. `std::unique_ptr`, `std::string`). The C API would be `extern "C"` with C-compatible types; that layer would call these. Header comment: "Non-template (type-erased) C++ API" (data_block.hpp ~1322).
- [x] **Public API** – Used by examples (`datahub_producer_example.cpp`, `datahub_consumer_example.cpp`), tests, and recovery; documented in READMEs. Intended for C API bridge implementation, bindings, or when types are not known at compile time.
- [x] **No schema generation** – No template, so no `generate_schema_info` call.
  - Code: `data_block.cpp` lines 3113–3118: calls `create_datablock_producer_impl(..., nullptr, nullptr)`.
- [x] **Impl zeros both schema hash fields when given null** – Header has zeroed schema hashes.
  - Code: `data_block.cpp` lines 3077–3080 and 3091–3095: `else { memset(header->flexzone_schema_hash, 0, ...); }` and same for `datablock_schema_hash`, `schema_version = 0`.

**Conclusion:** Template path stores schemas; non-template path does not. "Schema validation always there" is **not** true for the non-template API – schema storage is only done when the template API is used.

---

## 2. Consumer discovery

### 2.1 Template `find_datablock_consumer<FlexZoneT, DataBlockT>(..., expected_config)`

- [x] **Schema generation** – Both expected schemas generated from template types.
  - Code: `data_block.hpp` lines 1474–1476: `generate_schema_info<FlexZoneT>`, `generate_schema_info<DataBlockT>`.
- [x] **Schemas and config passed to impl** – Both schema pointers and config pointer passed.
  - Code: `data_block.hpp` lines 1479–1480: `find_datablock_consumer_impl(..., &expected_config, &expected_flexzone, &expected_datablock)`.
- [x] **Impl validates FlexZone hash when flexzone_schema != nullptr** – Compares header to expected.
  - Code: `data_block.cpp` `find_datablock_consumer_impl()`:
    - Lines 3179–3205: if `flexzone_schema != nullptr`, checks `has_flexzone_schema` then `memcmp(header->flexzone_schema_hash, flexzone_schema->hash.data(), ...)`; on mismatch **returns nullptr** (and increments `schema_mismatch_count`).
- [x] **Impl validates DataBlock hash when datablock_schema != nullptr** – Same pattern.
  - Code: `data_block.cpp` lines 3207–3245: if `datablock_schema != nullptr`, checks `has_datablock_schema`, then `memcmp(header->datablock_schema_hash, ...)`; on mismatch **returns nullptr**. Version check then; on failure **returns nullptr**.

### 2.2 Schema mismatch behavior: return vs throw

- [x] **Actual behavior on FlexZone/DataBlock schema mismatch** – Consumer impl **returns nullptr**; it does **not** throw.
  - Code: `data_block.cpp` lines 3192, 3201, 3221, 3230, 3240: all schema mismatch paths `return nullptr`.
- [ ] **Documentation vs code** – Header comment says "throws SchemaValidationException if schema hashes don't match" (`data_block.hpp` ~1458). **Incorrect:** impl returns `nullptr`. Only `validate_header_layout_hash(header)` (header ABI) can throw `SchemaValidationException`; that is caught and converted to `return nullptr` (lines 3151–3161).

**Conclusion:** Template consumer path validates both schemas; on mismatch it returns `nullptr`, not an exception. Doc should be updated to "returns nullptr if schema hashes don't match".

### 2.3 Non-template `find_datablock_consumer(..., config)` and `find_datablock_consumer(...)` (no config)

- [x] **Same C++ public API as producer** – Type-erased C++ overloads; public (examples, tests, recovery). Not the C API; a C API would wrap these.
- [x] **No schema validation** – Both call impl with `nullptr, nullptr` for schema pointers.
  - Code: `data_block.cpp` lines 3284–3289: with config → `find_datablock_consumer_impl(..., &expected_config, nullptr, nullptr)`; lines 3278–3282: no config → `find_datablock_consumer_impl(..., nullptr, nullptr, nullptr)`.
- [x] **Config validation** – When `expected_config != nullptr`, impl uses it (e.g. policy, sizes); when null, config checks are skipped for those branches. (Exact config checks are elsewhere in the same function; not re-listed here.)

---

## 3. SharedMemoryHeader schema fields

- [x] **Dual schema fields exist** – `flexzone_schema_hash[32]` and `datablock_schema_hash[32]`.
  - Code: `data_block.hpp` struct `SharedMemoryHeader`: `flexzone_schema_hash[32]`, `datablock_schema_hash[32]` (and `schema_version`).
- [x] **Header size 4096** – Enforced by static_assert.
  - Code: `data_block.hpp` `static_assert(raw_size_SharedMemoryHeader == 4096, "Header must be exactly 4KB")`.

---

## 4. Summary: What is actually guaranteed

| Claim | Verified? | Code reference |
|-------|-----------|----------------|
| Template producer stores both schema hashes | Yes | data_block.hpp 1441–1442 → data_block.cpp 3069–3095 |
| Template consumer validates both schema hashes | Yes | data_block.hpp 1479–1480 → data_block.cpp 3179–3245 |
| Schema mismatch → throw | No (doc error) | Impl returns nullptr; data_block.cpp 3192, 3201, 3221, 3230, 3240 |
| Non-template producer stores no schema | Yes | data_block.cpp 3118 (nullptr, nullptr) → 3077–3095 (zeros) |
| Non-template consumer validates no schema | Yes | data_block.cpp 3282, 3288 (nullptr, nullptr) |

---

## 5. Required documentation updates

- [ ] **data_block.hpp** – Change consumer template comment from "@throws SchemaValidationException if schema hashes don't match" to "Returns nullptr if schema hashes don't match (or if producer did not store schemas)."
- [ ] **API_SURFACE_DOCUMENTATION.md** / **PHASE4_DUAL_SCHEMA_API_DESIGN.md** – Add verification checkboxes and code references per DESIGN_VERIFICATION_RULE.md; fix consumer behavior to "returns nullptr" where applicable.

---

**Document Control**  
Created: 2026-02-15  
Updated: When code or design changes; re-verify and update checkboxes/code refs.
