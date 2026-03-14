# pyLabHub

**pyLabHub** is a cross-platform framework for high-performance scientific data acquisition, instrument control, and experiment management. It connects data-producing instruments -- sensors, DAQs, actuators -- to consuming applications -- storage, analysis, visualization -- at sub-millisecond latency, across process and language boundaries.

The framework is built on a **C++ core** (C++20, CMake 3.29+) with **Python scripting** for application logic. Data flows through shared memory at zero-copy speed; a lightweight ZeroMQ broker handles discovery, authentication, and liveness without touching the data path. User code lives in Python callbacks -- no C++ compilation needed for typical use.

**Design philosophy**: the platform guarantees delivery and invariant validation; protocol correctness is the responsibility of the application. Structural violations (schema mismatch, heartbeat timeout, SHM corruption) trigger log + notify + shutdown with no silent repair. Application-level issues (dead consumer, data checksum anomalies) are reported with a configurable response policy.

**Core principle**: data integrity and reproducibility. Raw experiment data is isolated from downstream analysis, ensuring the original record remains uncompromised while providing flexible tools for real-time visualization, control, and scripted automation.

## Target Use Cases

- **Lab automation** -- coordinate instruments and sensors in real-time experiments
- **Real-time sensing and control** -- stream measurements while dynamically adjusting hardware
- **Data pipelines** -- chain producers, processors, and consumers into transform pipelines
- **Cross-host bridging** -- relay data between machines via ZMQ with automatic endpoint discovery
- **Custom interfaces** -- integrate with Python notebooks, GUIs, or existing scientific software

## Quick Start

### Prerequisites

- GCC 12+ or Clang 15+ (C++20)
- CMake 3.29+
- System libraries: libzmq, libsodium

Third-party dependencies (fmt, nlohmann_json, googletest, cppzmq, luajit, pybind11) are bundled under `third_party/`.

### Build and test

```bash
git clone https://github.com/Qing-LAB/pylabhub.git
cd pylabhub
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build -j2
```

### Run the demo pipeline

```bash
bash share/py-demo-single-processor-shm/run_demo.sh   # hub + producer + processor + consumer
                                                        # Ctrl-C to stop all four
```

## The Four Binaries

pyLabHub ships four standalone executables. Each loads a Python script at startup and calls user-defined callbacks in a tight loop.

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
pylabhub-producer --config <path.json> --validate  # Validate config + script; exit 0/1
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
    if in_slot is None: return           # Timeout -- no data
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

## Architecture

### Library structure

| Library | CMake target | Type | Purpose |
|---------|-------------|------|---------|
| **pylabhub-utils** | `pylabhub::utils` | shared | All utilities — low-level primitives (SpinGuard, scope_guard) and high-level IPC (DataBlock, Messenger, BrokerService, Logger, Lifecycle). Pimpl ABI. |
| **pylabhub-scripting** | (static) | static | Python embedding (pybind11). Linked only by executables that embed Python. |

### API layers

| Layer | Header | What it provides |
|-------|--------|------------------|
| L0 | `plh_platform.hpp` | Platform detection macros, version API |
| L1 | `plh_base.hpp` | C++ primitives -- SpinGuard, format_tools, scope_guard |
| L2 | `plh_service.hpp` | RAII utilities -- Lifecycle, Logger, FileLock, DataBlock |
| L3a | `plh_datahub_client.hpp` | Client API -- hub::Producer, hub::Consumer, Messenger |
| L3b | `plh_datahub.hpp` | Full stack -- BrokerService, JsonConfig, HubConfig, SchemaLibrary |

### Five communication planes

| Plane | What flows | Mechanism |
|-------|-----------|-----------|
| **Data** | Slot payloads (raw bytes) | SHM ring buffer or ZMQ PUSH/PULL |
| **Control** | HELLO, BYE, REG, HEARTBEAT | ZMQ ROUTER-DEALER via Broker |
| **Message** | Arbitrary typed messages | ZMQ via Messenger (bidirectional) |
| **Timing** | Loop pacing | LoopPolicy on Producer/Consumer/Processor |
| **Metrics** | Counter snapshots, custom KV | Piggyback on HEARTBEAT + METRICS_REPORT_REQ |

## Pipeline Topologies

### Linear
```
Producer --[SHM]--> Consumer
```

### Transform
```
Producer --[SHM]--> Processor --[SHM]--> Consumer
```

### Cross-machine bridge
```
Host A: Producer --> Processor(bridge-out) --[ZMQ]-->
Host B: --[ZMQ]--> Processor(bridge-in) --> Consumer
```

### Chained transform
```
Producer --> P1(normalize) --> P2(filter) --> P3(compress) --> Consumer
```

### Fan-out
```
              +---> Consumer (monitor)
Producer --[SHM]---> Processor (archive)
              +---> Processor (analysis)
```

## Configuration Model

Each binary reads a flat JSON config. Schemas use the **BLDS** (Binary Layout Description Schema) format to define typed slot fields.

```json
{
  "hub_dir": "/path/to/hub",
  "producer": { "name": "TempSensor", "log_level": "info" },
  "channel": "lab.sensors.temperature",
  "target_period_ms": 100,
  "loop_timing": "fixed_rate",
  "slot_schema": {
    "packing": "aligned",
    "fields": [
      { "name": "ts",    "type": "float64" },
      { "name": "value", "type": "float32" }
    ]
  },
  "shm": { "enabled": true, "slot_count": 8 },
  "script": { "type": "python", "path": "." }
}
```

## Two Development Paths

### Python scripting (recommended)

Use `--init` to scaffold a directory, edit `script/python/__init__.py`, run. No C++ compilation needed. Hot-reload by restarting the binary.

### C++ RAII API

For embedding in custom applications, use the L2/L3 headers directly. See `examples/` for complete C++ examples (`cmake -DPYLABHUB_BUILD_EXAMPLES=ON`).

## Project Structure

```
pylabhub/
  src/                    # C++ source (libraries + 4 binaries)
  tests/                  # GoogleTest suite (1160+ tests)
  examples/               # C++ embedded API examples
  share/
    py-demo-single-processor-shm/   # SHM pipeline demo (4 processes)
    py-demo-dual-processor-bridge/  # Cross-hub SHM+ZMQ bridge demo (6 processes)
    py-examples/                    # Standalone Python script examples
    scripts/python/                 # Admin tools (hubshell_client.py)
  docs/
    HEP/                  # Design specifications (HEP-CORE-0001 through 0024)
    README/               # Detailed library and deployment docs
    tech_draft/           # Active design drafts
    todo/                 # Open work items by area
  third_party/            # Bundled dependencies (git submodules)
  cmake/                  # CMake helpers
  tools/                  # Developer tools (format.sh)
  CMakeLists.txt          # Top-level CMake build
  CLAUDE.md               # Build commands, project rules, architecture reference
  LICENSE                 # BSD 3-Clause
```

## Future Directions

The long-term vision extends beyond the current IPC core:

- **Data persistence** -- A dedicated archiver role (`pylabhub-archiver`) that writes channel data to Zarr (array streams) and Parquet (events/metadata) for long-term storage. Run manifests with SHA-256 integrity, provenance tracking, and DOI assignment. See `docs/tech_draft/future-persistence-and-discovery/` for early prototypes.
- **Hardware adapters** -- Unified drivers for sensors, actuators, and DAQs that bridge instruments into hub channels.
- **Hub discovery** -- A domain service that maintains a registry of available hubs, channels, and roles for dynamic routing.
- **Language connectors** -- Python client library (via pybind11), Igor Pro bridge, Jupyter integration.
- **Multi-host operation** -- Secure cross-network pipelines with CurveZMQ authentication and federation.

## Further Reading

| Document | Description |
|----------|-------------|
| `docs/README/` | Detailed library and build documentation |
| `docs/HEP/` | Authoritative design specifications |
| `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md` | SHM layout, protocol, architecture layers |
| `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md` | Pipeline topologies and communication planes |
| `share/py-demo-single-processor-shm/` | Working four-process SHM demo |
| `share/py-demo-dual-processor-bridge/` | Working six-process cross-hub bridge demo |
| `examples/` | C++ RAII API examples |
| `CLAUDE.md` | Build commands, project rules, architecture reference |

## License

This project is licensed under the [BSD 3-Clause License](LICENSE).
(c) 2025 Quan Qing, Arizona State University
