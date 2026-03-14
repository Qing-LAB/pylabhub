# PyLabHub C++ Examples

These examples demonstrate the C++ API directly, without Python scripting. All examples link
against `pylabhub::utils` (shared library) and are compiled as standalone executables.

## Building

Examples are **not** built by default. Enable with the `PYLABHUB_BUILD_EXAMPLES` CMake option:

```bash
cmake -S . -B build -DPYLABHUB_BUILD_EXAMPLES=ON
cmake --build build
```

Binaries land in `build/stage-<buildtype>/bin/example_*`.

## Example Inventory

### Hub Service

- **hub_active_service_example.cpp** (`example_hub_active_service`) — Connects to a running
  hub broker, registers as an active service, and handles control events.

- **hub_health_example.cpp** (`example_hub_health`) — Queries hub health and peer status
  via the admin socket.

### DataHub Producer / Consumer

- **datahub_producer_example.cpp** (`example_datahub_producer`) — Creates a DataBlock
  producer using `create_datablock_producer<F,D>` and the RAII `with_transaction` / `ctx.slots()` API.

- **datahub_consumer_example.cpp** (`example_datahub_consumer`) — Attaches a DataBlock
  consumer using `find_datablock_consumer<F,D>` and reads slots via `with_transaction`.

### Processor Pipeline

- **cpp_processor_template.cpp** (`example_processor_pipeline`) — Full
  Producer → ShmQueue → Processor → Consumer pipeline in a single C++ binary. Use this
  as a starting point for C++-only processor development.

### RAII Layer

- **raii_layer_example.cpp** (`example_raii_layer`) — Modern C++20 RAII Layer with
  type-safe transactions. Covers `with_transaction<FlexZoneT, DataBlockT>()`, the
  `Result<T,E>` error model (use `.content()`, not `.value()`), `SlotRef.get()`,
  `ZoneRef.get()`, schema validation, and heartbeat management.
  **Recommended for all new C++ code.**

- **RAII_LAYER_USAGE_EXAMPLE.md** — Comprehensive usage guide with patterns and best practices.

### Schema Registry

- **schema_example.cpp** (`example_schema`) — BLDS schema registration via
  `PYLABHUB_SCHEMA_*` macros, `generate_schema_info<>()`, and `validate_schema_match()`.
