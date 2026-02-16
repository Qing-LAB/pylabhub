# C++ DataHub examples

# DataHub C++ Examples

These examples demonstrate PyLabHub DataHub usage patterns with the **current API** (Phase 3 RAII Layer).

## ✨ Recommended: RAII Layer (Phase 3)

- **raii_layer_example.cpp** — **NEW!** Modern C++20 RAII Layer with type-safe transactions
  - `with_transaction<FlexZoneT, DataBlockT>()` entry points
  - Non-terminating `SlotIterator` with `Result<T, E>` error handling
  - Type-safe `SlotRef.get()` and `ZoneRef.get()` access
  - Use `.content()` to extract from Result (not `.value()`)
  - Schema validation and heartbeat management
  - **Use this for all new code!**

- **RAII_LAYER_USAGE_EXAMPLE.md** — Comprehensive usage guide with patterns and best practices

## Legacy Examples (Old API - To Be Updated)

- **datahub_producer_example.cpp** — ⚠️ Uses deprecated `with_write_transaction`, `with_typed_write`, `WriteTransactionGuard`
- **datahub_consumer_example.cpp** — ⚠️ Uses deprecated `slot_iterator().try_next()`, `with_next_slot`, `with_read_transaction`
- **schema_example.cpp** — BLDS schema registration (`PYLABHUB_SCHEMA_*`), `generate_schema_info`, `validate_schema_match`

## Building Examples

Examples are **not** built by the default CMake configuration. To build and run, add an `add_executable` in a CMakeLists that links `pylabhub::utils` and the same dependencies as the Layer 3 tests (fmt, nlohmann_json, libzmq, libsodium, luajit). See `docs/README/README_testing.md` and the test layer structure for reference.

## Migration Note

When updating legacy examples, use the **Phase 3 RAII API**:
- Replace `with_write_transaction()` → `producer->with_transaction<FlexZone, Message>()`
- Replace `DataBlockSlotIterator` → `ctx.slots()` iterator
- Replace `slot_result.value()` → `slot_result.content()`

See `RAII_LAYER_USAGE_EXAMPLE.md` for migration patterns.
