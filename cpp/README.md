# pyLabHub C++

High-performance, zero-copy IPC framework for scientific data acquisition.
C++20, CMake 3.29+, 828 tests.

pyLabHub connects instruments, processors, and analysis tools through shared
memory with microsecond-level latency. A lightweight broker handles discovery
and authentication while staying out of the data path.

## Quick Start

### Prerequisites

- GCC 12+ or Clang 15+ (C++20)
- CMake 3.29+
- System libraries: libzmq, libsodium

Third-party dependencies (fmt, nlohmann_json, googletest, cppzmq, luajit,
pybind11) are bundled under `third_party/`.

### Build and test

```bash
cd cpp/
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build -j2
```

### Run the demo pipeline

```bash
bash share/demo/run_demo.sh     # hub + producer + processor + consumer
                                # Ctrl-C to stop all four
```

## Architecture

### Dual library structure

| Library | CMake target | Type | Purpose |
|---------|-------------|------|---------|
| **pylabhub-basic** | `pylabhub::basic` | static | In-process primitives (SpinGuard, scope_guard, format tools). Zero dependencies. |
| **pylabhub-utils** | `pylabhub::utils` | shared | High-level IPC (DataBlock, Messenger, BrokerService, Logger, Lifecycle). Pimpl ABI. |

Always link against alias targets (`pylabhub::utils`, not `pylabhub-utils`).

### API layers and umbrella headers

| Layer | Header | What it provides |
|-------|--------|------------------|
| L0 | `plh_platform.hpp` | Platform detection macros, version API |
| L1 | `plh_base.hpp` | C++ primitives — SpinGuard, format_tools, scope_guard, recursion_guard |
| L2 | `plh_service.hpp` | RAII utilities — Lifecycle, Logger, FileLock, DataBlockProducer/Consumer |
| L3a | `plh_datahub_client.hpp` | Client API — hub::Producer, hub::Consumer, Messenger |
| L3b | `plh_datahub.hpp` | Full stack — BrokerService, JsonConfig, HubConfig, SchemaLibrary |

Include one header per level. Each layer includes all lower layers transitively.

### Key design patterns

- **Pimpl idiom** — All public classes in `pylabhub-utils` hide members behind an
  `Impl` struct for ABI stability.
- **Lifecycle management** — Modules register a `ModuleDef`; `LifecycleGuard` in
  `main()` handles topological-sort init and reverse-order teardown.
- **DataBlock (shared memory)** — Single SHM segment per channel with ring buffer
  of typed data slots. Two-tier sync: OS-backed mutex (control zone) + atomic
  PID-based spinlock (data slots). Zero-copy reads via `std::span`.

## The Four Binaries

pyLabHub ships four standalone executables. Each loads a Python script at
startup and calls user-defined callbacks in a tight loop.

| Binary | Config | Purpose |
|--------|--------|---------|
| `pylabhub-hubshell` | `hub.json` | Broker service + Python admin shell |
| `pylabhub-producer` | `producer.json` | Writes slots to one channel on a timer |
| `pylabhub-consumer` | `consumer.json` | Reads slots from one channel on demand |
| `pylabhub-processor` | `processor.json` | Reads from channel A, transforms, writes to channel B |

### CLI (shared across producer, consumer, processor)

```
pylabhub-producer --init [<dir>] [--name <name>]   # Scaffold config + vault + script
pylabhub-producer <dir>                            # Run from directory
pylabhub-producer --config <path.json>             # Run from explicit config file
pylabhub-producer --config <path.json> --validate  # Validate config + script; exit 0/1
pylabhub-producer --config <path.json> --keygen    # Generate CurveZMQ keypair; exit 0
pylabhub-producer --version                        # Print version
```

### Python callbacks

**Producer** (`import pylabhub_producer as prod`)
```python
def on_init(api):      pass             # Called once at startup
def on_produce(out_slot, flexzone, messages, api) -> bool:
    out_slot.value = 42.0               # Fill typed slot fields
    return True                          # True = commit, False = discard
def on_stop(api):      pass             # Called on shutdown
```

**Consumer** (`import pylabhub_consumer as cons`)
```python
def on_init(api):      pass
def on_consume(in_slot, flexzone, messages, api):
    if in_slot is None: return           # Timeout — no data
    print(in_slot.value)                 # Read-only typed fields
def on_stop(api):      pass
```

**Processor** (`import pylabhub_processor as proc`)
```python
def on_init(api):      pass
def on_process(in_slot, out_slot, flexzone, messages, api) -> bool:
    if in_slot is None: return False     # Timeout
    out_slot.result = in_slot.value * 2  # Transform
    return True
def on_stop(api):      pass
```

## Configuration Model

Each binary reads a flat JSON config. Schemas use the **BLDS** (Binary Layout
Description Schema) format to define typed slot fields.

```json
{
  "producer": { "name": "TempSensor", "log_level": "info" },
  "broker": "tcp://127.0.0.1:5570",
  "channel": "lab.sensors.temperature",
  "interval_ms": 100,
  "slot_schema": {
    "packing": "natural",
    "fields": [
      { "name": "ts",    "type": "float64" },
      { "name": "value", "type": "float32" }
    ]
  },
  "shm": { "enabled": true, "slot_count": 8 },
  "script": { "type": "python", "path": "." }
}
```

### UID format

Each binary generates a unique identity: `PROD-TEMPSENSOR-A3F7C219`.
Format: `{PREFIX}-{NAME}-{8HEX}`.

### Script directory convention

All binaries resolve scripts as `<script.path>/script/<script.type>/__init__.py`.
With the default `"path": "."`, the entry point is `./script/python/__init__.py`.

### Named schemas

Schema IDs like `lab.sensors.temperature.raw@1` map to files under
`schemas/lab/sensors/temperature.raw.v1.json`. The BLAKE2b-256 hash of the
BLDS is the primary identity; the name is a human alias.

## Two Development Paths

### Python scripting (recommended)

Use `--init` to scaffold a directory, edit `script/python/__init__.py`, run.
No C++ compilation needed. Hot-reload by restarting the binary.

### C++ RAII API

For embedding in custom applications, use the L2/L3 headers directly:

```cpp
#include <plh_datahub.hpp>

// Producer side
auto producer = create_datablock_producer<FlexT, SlotT>(name, policy, config);
producer->with_transaction<FlexT, SlotT>(timeout, [](auto& ctx) {
    for (auto&& result : ctx.slots(slot_timeout)) {
        if (!result.is_ok()) break;
        auto& slot = result.content().get();
        slot.value = 42.0f;
    }
});

// Or use hub::Processor for queue-based pipelines
auto proc = hub::Processor::create(in_queue, out_queue, opts);
proc->set_process_handler<InF, InD, OutF, OutD>(
    [](hub::ProcessorContext<InF, InD, OutF, OutD>& ctx) {
        ctx.output().value = ctx.input().value * 2;
        return true;
    });
proc->start();
```

See `examples/` for complete C++ examples (`cmake -DPYLABHUB_BUILD_EXAMPLES=ON`).

## Pipeline Topologies

### Linear

```
Producer ──[SHM]──▶ Consumer
```

Single host, one channel. The simplest configuration.

### Transform

```
Producer ──[SHM]──▶ Processor ──[SHM]──▶ Consumer
```

Processor reads from input channel, transforms each slot, writes to output channel.

### Cross-machine bridge

```
Host A: Producer ──▶ Processor(bridge-out) ──[ZMQ]──▶
Host B: ──[ZMQ]──▶ Processor(bridge-in) ──▶ Consumer
```

Two bridge processors relay data over the network via ZMQ PUSH/PULL.

### Chained transform

```
Producer ──▶ P1(normalize) ──▶ P2(filter) ──▶ P3(compress) ──▶ Consumer
```

Multiple processors in series, each with independent SHM channels.

### Fan-out

```
              ┌──▶ Consumer (monitor)
Producer ──[SHM]──▶ Processor (archive)
              └──▶ Processor (analysis)
```

One producer, multiple readers. SHM ring buffer supports concurrent consumers.

## Getting Started

Minimal producer + consumer walkthrough using three terminals.

### 1. Start the hub

```bash
pylabhub-hubshell --dev
```

### 2. Set up and run a producer

```bash
pylabhub-producer --init my_producer --name TempSensor
# Edit my_producer/producer.json: set channel, broker, schema
# Edit my_producer/script/python/__init__.py: implement on_produce()
pylabhub-producer my_producer
```

### 3. Set up and run a consumer

```bash
pylabhub-consumer --init my_consumer --name TempMonitor
# Edit my_consumer/consumer.json: match channel + schema from producer
# Edit my_consumer/script/python/__init__.py: implement on_consume()
pylabhub-consumer my_consumer
```

For a working example with all configs pre-filled, see `share/demo/`.

## Five Communication Planes

| Plane | What flows | Mechanism |
|-------|-----------|-----------|
| **Data** | Slot payloads (raw bytes) | SHM ring buffer or ZMQ PUSH/PULL |
| **Control** | HELLO, BYE, REG, HEARTBEAT | ZMQ ROUTER–DEALER via Broker |
| **Message** | Arbitrary typed messages | ZMQ via Messenger (bidirectional) |
| **Timing** | Loop pacing | LoopPolicy on Producer/Consumer |
| **Metrics** | Counter snapshots, custom KV | Piggyback on HEARTBEAT + METRICS_REPORT_REQ → Broker aggregation |

## Further Reading

| Document | Description |
|----------|-------------|
| `docs/README/` | Detailed library and build documentation |
| `docs/HEP/` | Authoritative design specifications (HEP-CORE-0001 through 0020) |
| `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md` | SHM layout, protocol, architecture layers |
| `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md` | Pipeline topologies and communication planes |
| `share/demo/` | Working four-process demo with pre-filled configs |
| `examples/` | C++ RAII API examples (opt-in: `-DPYLABHUB_BUILD_EXAMPLES=ON`) |
| `CLAUDE.md` | Build commands, project rules, architecture reference |
