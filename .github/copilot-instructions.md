## Purpose
Short, practical guidance for an AI coding agent working on pyLabHub: where the logic lives, how the pieces talk, and the concrete commands and code examples to be useful immediately.

## Big picture (core components)
- Hub (central): `pylabhub/hub/api.py` — in‑process pub/sub with three channels: `control` (commands/acks), `events` (logs/state), and `data` (high throughput). Messages are envelopes with `header` (must include `msg_id`, `ts` with wall+monotonic, `trace_id`) and optional `buffer` for binary payloads.
- Persistence service: `pylabhub/hub/persistence_service.py` — example implementation that writes `events/state` → Parquet and `data` → Zarr. Look here for expected header fields (`dtype`, `shape`, `stream`) and concurrency patterns (asyncio.Lock).
- Manifests: `pylabhub/hub/manifest.py` — CLI helpers to `init`, `finalize`, and `validate` run manifests. Finalize computes per-stream SHA‑256 and a manifest self-hash.

## Key developer workflows (concrete commands)
- Python dev environment: `pip install -e .` from the repo root (this installs the `pylabhub-manifest` script).
- Run the provided demo (package must be importable):
  - `python -m pylabhub.hub.example.example_persistence_service`
- Manifest CLI usage (examples found in `manifest.py`):
  - `pylabhub-manifest init --run-dir runs/run-001 --run-id run-001`
  - `pylabhub-manifest finalize --manifest runs/run-001/manifest.yaml --run-dir runs/run-001`
  - `pylabhub-manifest validate --manifest runs/run-001/manifest.yaml`
- Tests: run `pytest` from the repository root (see `tests/test_manifest.py` for the manifest contract).
- C/C++ build (optional component): `cpp/CMakePresets.json` is included — example:
  - `cd cpp`
  - `cmake --preset linux-release`
  - `cmake --build --preset build`
  - Use `cmake --preset test` or run `ctest` in the build dir to run C++ tests.

## Project-specific patterns and expectations
- Hub semantics: control messages expect deterministic ack correlation by `msg_id`. The Hub registers ack waiters; if no handler posts an ack the API returns an optimistic OK. See `Hub.send_control`, `await_ack`, and `post_ack` in `api.py`.
- Message header shape: prefer including `msg_id`, `trace_id`, `hub_id`, `ts` (both `wall_clock` and `monotonic`), `schema`, and channel-specific fields (`event`, `scope`, `stream`, `dtype`, `shape`, `encoding`). See `StreamSpec` and usage in `pylabhub/hub/example/example_persistence_service.py`.
- Persistence expectations:
  - Events/state: flattened rows written to Parquet. Payloads should be JSON-serializable and placed under `payload` in the header.
  - Data streams: messages must provide `dtype` and `shape` to reconstruct numpy arrays from `buffer` bytes; persistence appends each message as one sample along axis 0 in Zarr.
- Manifest conventions: `schema_version`, `run`, `acquisition.streams` with `name/kind/format/path` are required; `finalize_manifest` fills `integrity.manifest_sha256` and per-stream sha256s.
- Optional dependencies: persistence demo imports `numpy`, `pyarrow`, and `zarr` but the project `pyproject.toml` only requires `pyyaml`. Install extras when working on persistence features: `pip install numpy pyarrow zarr`.

## Important files to open first
- `pylabhub/hub/api.py` — Hub API and message model
- `pylabhub/hub/persistence_service.py` — example backing store (Parquet + Zarr), shows file layout and locks
- `pylabhub/hub/manifest.py` — manifest lifecycle and CLI
- `pylabhub/hub/example/example_persistence_service.py` — short runnable demo showing typical Hub usage
- `tests/test_manifest.py` — executable specification of expected manifest behaviour
- `cpp/CMakePresets.json` — how the C++ components are configured/built
- `README.md` — high‑level design, run layout and target formats

## Quick implementation tips for common tasks
- Adding a new Service: implement `id` and `async def on_message(self, msg)` and register with `hub.register_service(...)`.
- Emitting binary frames (images/waveforms): call `hub.send_data(stream, header={"dtype":"uint8","shape":(h,w)}, buffer=bytes)`; persistence expects `dtype` + `shape` to decode.
- Creating a run manifest in code/tests: use `manifest.init_manifest()` then add acquisition.streams entries and call `manifest.finalize_manifest()` to compute hashes.

## Known caveats & gotchas
- `manifest.py` falls back to JSON when PyYAML is not installed — but YAML read/write functions will raise if YAML is required and PyYAML is absent. The `requirements.txt` already lists `pyyaml`.
- Hub `await_ack` returns an optimistic OK if no waiter was registered; tests rely on finalize/validate behaviour rather than real ack roundtrips.
- The C++ tree includes vendored/third‑party libzmq. Building native code may require typical platform dev tools (Xcode on macOS) and Ninja/CMake versions matching `CMakePresets.json`.

## If something is missing
- If you need more context for a new feature (adapters, connectors, persistence formats), open the above files and the `docs/` and `cpp/` folders; tests provide living examples of expected behavior. Ask for a focused walk‑through on any one component and I will expand the instructions.

---
Please review these instructions and tell me which sections need more detail or which workflows you'd like made into scripts/CI steps.
