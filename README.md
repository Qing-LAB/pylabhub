# pyLabHub

**pyLabHub** is a modular framework for laboratory data acquisition, hardware control, and experiment management. Its design revolves around a **central hub** for data streaming and coordination, **producers** that bridge instruments into the data plane, **consumers** that receive and act on data, and **processors** that transform data between channels.

The core principle is **data integrity and reproducibility**. Raw experiment data is isolated from downstream analysis, ensuring the original record remains uncompromised. At the same time, the framework provides flexible tools for real-time visualization, control, and scripted automation through Python callbacks that respond dynamically to streaming data.

The framework is built on a **C++ core** (C++20) with **Python scripting** for application logic. Data flows through shared memory at zero-copy speed; a lightweight ZeroMQ broker handles discovery, authentication, and liveness without touching the data path. User code lives in Python callbacks -- no C++ compilation needed for typical use.

---

## Target Use Cases

- **Lab automation** -- coordinate instruments and sensors in real-time experiments
- **Real-time sensing and control** -- stream measurements while dynamically adjusting hardware
- **Data pipelines** -- chain producers, processors, and consumers into transform pipelines
- **Data archiving** -- capture raw experiment data in open, long-term formats (Zarr, Parquet, HDF5)
- **Cross-host bridging** -- relay data between machines via ZMQ with automatic endpoint discovery
- **Collaborative research** -- share data and analyses without compromising original records
- **Custom interfaces** -- integrate with Python notebooks, GUIs, or existing scientific software (Igor Pro, Jupyter)

---

## How It Works

pyLabHub operates as a set of cooperating processes, each with a clear role:

```
                         Hub (broker)
                        /     |      \
                       /      |       \
              Producer    Processor    Consumer
           (instrument)  (transform)  (storage/display)
```

- **Hub** (`pylabhub-hubshell`) -- The central broker. Manages channel registration, heartbeats, authentication, and role discovery. Does not touch the data path. Runs an optional Python admin shell for interactive control.

- **Producer** (`pylabhub-producer`) -- Acquires data from an instrument or source and writes it to a named channel. Data flows via shared memory (zero-copy, sub-millisecond) or ZMQ (cross-host). A Python callback (`on_produce`) fills each slot on a configurable timer.

- **Consumer** (`pylabhub-consumer`) -- Reads data from a channel. Used for storage, visualization, logging, or any downstream action. A Python callback (`on_consume`) receives each slot as it arrives.

- **Processor** (`pylabhub-processor`) -- Reads from one channel, transforms the data, and writes to another. Used for filtering, calibration, format conversion, or bridging between hosts. A Python callback (`on_process`) receives an input slot and fills an output slot.

Each role loads a Python script at startup and calls user-defined callbacks in a tight loop. The C++ runtime handles all IPC, synchronization, error recovery, and lifecycle management.

### Communication Planes

| Plane | What flows | Mechanism |
|-------|-----------|-----------|
| **Data** | Slot payloads (raw bytes) | SHM ring buffer or ZMQ PUSH/PULL |
| **Control** | HELLO, BYE, REG, HEARTBEAT | ZMQ ROUTER-DEALER via Broker |
| **Message** | Arbitrary typed messages | ZMQ via Messenger (bidirectional) |
| **Timing** | Loop pacing | LoopPolicy on Producer/Consumer/Processor |
| **Metrics** | Counter snapshots, custom KV | Piggyback on HEARTBEAT + METRICS_REPORT_REQ |

### Pipeline Topologies

```
Linear:         Producer --[SHM]--> Consumer

Transform:      Producer --[SHM]--> Processor --[SHM]--> Consumer

Chained:        Producer --> P1(normalize) --> P2(filter) --> Consumer

Cross-host:     Host A: Producer --> Processor --[ZMQ]-->
                Host B: --> Processor --> Consumer

Fan-out:        Producer --+--> Consumer (monitor)
                           +--> Processor (archive)
                           +--> Processor (analysis)
```

---

## Installation

### Option A: Install from PyPI (recommended for users)

```bash
pip install pylabhub
```

This installs prebuilt binaries, the shared library, public headers, and a bundled Python runtime. After installation the four executables and the `pylabhub-pyenv` environment tool are available on PATH.

### Option B: Build from source (for developers)

**Prerequisites**: GCC 12+ or Clang 15+ (C++20), CMake 3.29+, Ninja (recommended)

All third-party dependencies are built automatically from bundled sources.

```bash
git clone --recursive https://github.com/Qing-LAB/pylabhub.git
cd pylabhub
cmake -S . -B build
cmake --build build
ctest --test-dir build                    # run 1160+ tests
```

### Install to a target directory

```bash
cmake --build build --target stage_all    # stage all artifacts
cmake --install build --prefix /opt/pylabhub   # install to target
```

### Python environment management

The bundled Python runtime ships with the interpreter and stdlib but **no third-party packages**. Use `pylabhub-pyenv` to manage packages post-install:

```bash
pylabhub-pyenv install                    # install default packages (numpy, zarr, h5py, ...)
pylabhub-pyenv create-venv daq-env        # create an isolated venv
pylabhub-pyenv install --venv daq-env -r req.txt
pylabhub-pyenv info                       # show environment details
```

---

## Quick Start

### Scaffold and run a producer

```bash
pylabhub-producer --init my-sensor --name TempSensor
cd my-sensor
# edit script/python/__init__.py
pylabhub-producer .
```

### Run the demo pipeline

```bash
bash share/py-demo-single-processor-shm/run_demo.sh   # hub + producer + processor + consumer
                                                        # Ctrl-C to stop all four
```

---

## Python Callbacks

Each role loads `script/python/__init__.py` and calls user-defined functions:

**Producer** (`import pylabhub_producer as prod`)
```python
def on_init(api):      pass
def on_produce(out_slot, flexzone, messages, api) -> bool:
    out_slot.value = 42.0               # fill typed slot fields
    return True                          # True = commit, False = discard
def on_stop(api):      pass
```

**Consumer** (`import pylabhub_consumer as cons`)
```python
def on_init(api):      pass
def on_consume(in_slot, flexzone, messages, api):
    if in_slot is None: return           # timeout -- no data
    print(in_slot.value)                 # read-only typed fields
def on_stop(api):      pass
```

**Processor** (`import pylabhub_processor as proc`)
```python
def on_init(api):      pass
def on_process(in_slot, out_slot, flexzone, messages, api) -> bool:
    if in_slot is None: return False
    out_slot.result = in_slot.value * 2  # transform
    return True
def on_stop(api):      pass
```

---

## Deployed Directory Structure

After `cmake --install` or `pip install`, the deployment layout is:

```
<prefix>/
  bin/
    pylabhub-hubshell           # broker + admin shell
    pylabhub-producer           # data source role
    pylabhub-consumer           # data sink role
    pylabhub-processor          # transform role
    pylabhub-pyenv              # Python environment manager
  lib/
    libpylabhub-utils-stable.so # shared library (IPC, SHM, broker, logging)
  include/
    plh_platform.hpp            # L0: platform detection
    plh_base.hpp                # L1: C++ primitives
    plh_service.hpp             # L2: lifecycle, logger, file lock
    plh_datahub.hpp             # L3: full data hub stack
    utils/                      # internal headers
  opt/
    python/                     # bundled Python 3.14 runtime
    luajit/                     # LuaJIT runtime (internal)
  share/
    py-demo-*/                  # demo pipelines
    scripts/python/             # default requirements.txt
  docs/
    HEP/                        # design specifications
    README/                     # detailed documentation
```

A working deployment directory for a role looks like:

```
my-sensor/
  hub.json                      # (or producer.json, consumer.json, processor.json)
  script/python/__init__.py     # user callbacks
  vault/                        # CurveZMQ keys (auto-generated)
```

---

## Configuration

Each binary reads a flat JSON config. Slot schemas use the **BLDS** (Binary Layout Description Schema) format:

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

---

## Data Persistence Strategy

pyLabHub uses a hybrid approach to balance efficiency, openness, and reproducibility:

- **Zarr** for multidimensional array streams (waveforms, images). Chunked by time and channel, compressed (Blosc + Zstd), stored in a directory layout that works locally and in cloud object stores.
- **Parquet** for event logs, metadata, and control records. Time-sliced files queryable with pandas/Arrow/DuckDB.
- **HDF5 export** for compatibility with existing scientific tools, while keeping Zarr/Parquet as native formats.

```
runs/
  run-2025-09-18-001/
    arrays/
      raw_adc.zarr/
      camera_frames.zarr/
    events/
      control-20250918-120000.parquet
      telemetry-20250918-120000.parquet
    manifest.yaml   # metadata, checksums, provenance
```

---

## Two Development Paths

### Python scripting (recommended)

Use `--init` to scaffold a directory, edit `script/python/__init__.py`, run. No C++ compilation needed. Hot-reload by restarting the binary.

### C++ RAII API

For embedding in custom applications, use the L2/L3 headers directly:

```cpp
#include <plh_datahub.hpp>
// See examples/ for complete C++ examples (cmake -DPYLABHUB_BUILD_EXAMPLES=ON)
```

---

## Future Directions

- **Lua scripting** -- Lua script host integration is in progress, enabling lightweight callbacks alongside Python for latency-critical paths.
- **Archiver role** -- A dedicated `pylabhub-archiver` that writes channel data to Zarr/Parquet with run manifests, SHA-256 integrity, provenance tracking, and DOI assignment.
- **Hardware adapters** -- Unified drivers for sensors, actuators, and DAQs that bridge instruments into hub channels.
- **Hub discovery** -- A domain service maintaining a registry of available hubs, channels, and roles for dynamic routing.
- **Language connectors** -- Python client library, Igor Pro bridge, Jupyter integration.
- **Multi-host operation** -- Secure cross-network pipelines with CurveZMQ authentication and federation.
- **NSF/NIH alignment** -- Persistent identifiers, ORCID/ROR metadata, DMP generation, PROV/RO-Crate provenance, and repository export tooling.

---

## Further Reading

| Document | Description |
|----------|-------------|
| `docs/README/` | Detailed library, deployment, and API documentation |
| `docs/HEP/` | Authoritative design specifications (HEP-0001 through HEP-0025) |
| `share/py-demo-single-processor-shm/` | Working four-process SHM demo |
| `share/py-demo-dual-processor-bridge/` | Working six-process cross-hub bridge demo |
| `examples/` | C++ RAII API examples |
| `CLAUDE.md` | Build commands, project rules, architecture reference |

---

## License

This project is licensed under the [BSD 3-Clause License](LICENSE).
(c) 2025 Quan Qing, Arizona State University
