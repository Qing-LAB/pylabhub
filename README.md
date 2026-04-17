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

## Installation

### Option A: Install from PyPI (recommended for users)

```bash
pip install pylabhub
```

This installs prebuilt binaries, the shared library, and public headers. After installation the four executables (`pylabhub-hubshell`, `-producer`, `-consumer`, `-processor`) and the `plh_pyenv` tool are available on PATH.

**Download the Python 3.14 runtime** (required, not included in the wheel to keep it under PyPI size limits):

```bash
pylabhub prepare-runtime                        # downloads ~130 MB from GitHub
pylabhub prepare-runtime --from archive.tar.gz  # offline/air-gapped install
```

**Install Python packages for scripting** (optional):

```bash
plh_pyenv install                          # default requirements.txt
plh_pyenv install -r my-requirements.txt   # custom set
```

**Python path accessors** (for programmatic use):

```python
import pylabhub
pylabhub.get_bin_dir()        # path to executables
pylabhub.get_lib_dir()        # path to shared libraries
pylabhub.get_include_dir()    # path to C++ headers
pylabhub.runtime_available()  # True if Python 3.14 runtime is installed
```

### Option B: Build from source (for developers)

**Prerequisites**: GCC 12+ or Clang 15+ (C++20), CMake 3.29+

Third-party dependencies (fmt, nlohmann_json, googletest, cppzmq, luajit, pybind11, libzmq, libsodium) are built automatically from bundled sources under `third_party/`.

```bash
git clone --recursive https://github.com/Qing-LAB/pylabhub.git
cd pylabhub
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build -j2
```

Build outputs go to `build/stage-debug/` (or `stage-release/`) with `bin/`, `lib/`, `tests/`, `include/` subdirectories.

### Python environment management

After `pylabhub prepare-runtime`, the Python 3.14 runtime has the interpreter and stdlib but **no third-party packages**. Use `plh_pyenv` to manage packages:

```bash
plh_pyenv install                          # install default packages (numpy, zarr, h5py, ...)
plh_pyenv create-venv daq-env              # create an isolated venv
plh_pyenv install --venv daq-env -r req.txt  # install packages into venv
plh_pyenv info                             # show environment details
plh_pyenv verify                           # check all required packages present
```

Role configs can target a specific venv: `"python_venv": "daq-env"`. See `docs/README/README_Deployment.md` §12 for details.

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
  src/
    pylabhub/             # Python package (pip install shim + path accessors)
    ...                   # C++ source (libraries + 4 binaries)
  tests/                  # GoogleTest suite (1160+ tests)
  examples/               # C++ embedded API examples
  share/
    py-demo-single-processor-shm/   # SHM pipeline demo (4 processes)
    py-demo-dual-processor-bridge/  # Cross-hub SHM+ZMQ bridge demo (6 processes)
    py-examples/                    # Standalone Python script examples
    scripts/python/                 # Admin tools + requirements.txt
  docs/
    HEP/                  # Design specifications (HEP-CORE-0001 through 0025)
    README/               # Detailed library and deployment docs
    tech_draft/           # Active design drafts
    todo/                 # Open work items by area
  third_party/            # Bundled dependencies (git submodules + ExternalProject)
  tools/                  # plh_pyenv, format.sh
  cmake/                  # CMake helpers (staging, platform, third-party)
  pyproject.toml          # scikit-build-core config (pip install → wheel)
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
