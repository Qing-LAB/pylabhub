# C++ DataHub examples

These examples use the **current** C++ API and single memory layout (single flex zone, 4K-aligned; see HEP-CORE-0002 §3 and IMPLEMENTATION_GUIDANCE).

- **datahub_producer_example.cpp** — Lifecycle, `MessageHub::get_instance()`, `DataBlockConfig`, `create_datablock_producer`, `with_write_transaction`, `with_typed_write`, `WriteTransactionGuard`.
- **datahub_consumer_example.cpp** — `find_datablock_consumer` with `expected_config`, `slot_iterator().try_next()`, `with_next_slot`, `with_read_transaction`, `last_slot_id()`.
- **schema_example.cpp** — BLDS schema registration (`PYLABHUB_SCHEMA_*`), `generate_schema_info`, `validate_schema_match` (schema_blds.hpp).
- **RAII_LAYER_USAGE_EXAMPLE.md** — Usage narrative and code snippets for init, flexible zone, typed slot, and RAII patterns.

Examples are **not** built by the default CMake configuration. To build and run, add an `add_executable` in a CMakeLists that links `pylabhub::utils` and the same dependencies as the Layer 3 tests (fmt, nlohmann_json, libzmq, libsodium, luajit). See `docs/README/README_testing.md` and the test layer structure for reference.

When the API or memory layout changes, update these files so they stay accurate (see `docs/DOC_STRUCTURE.md` §1.6).
