# Future: Persistence Service & Hub Discovery

These files are early Python prototypes from the original pyLabHub design (Sep 2025).
They are preserved here as design references for future role development.

## Files

| File | Purpose |
|------|---------|
| `api.py` | Async Python Hub skeleton: 3-channel message bus (control/events/data), adapter/service registration, SHM stubs |
| `persistence_service.py` | Zarr (array streams) + Parquet (events/state) writer, async service interface |
| `example_persistence_service.py` | Demo: Hub + PersistenceService wiring |
| `manifest.py` | Run manifest (YAML/JSON): init/finalize/validate, SHA-256 tree hashing, stream integrity |

## Design ideas to carry forward

1. **Persistence role**: A new pylabhub binary (e.g., `pylabhub-archiver`) that connects as a
   consumer to one or more channels and writes Zarr/Parquet files. The manifest.py tooling
   (run manifests, stream hashing, integrity checks) can be adapted for this.

2. **Hub discovery / domain service**: A role that maintains a registry of available hubs,
   their channels, and connected roles. Enables cross-hub routing without hardcoded endpoints.
   The `api.py` session/registry model is a starting point.

3. **Run manifest**: Per-experiment metadata (investigator, instruments, streams, provenance,
   integrity checksums). See the original root README.md (archived) for the full schema draft.

## Status

These are **design references only** -- the code is not functional with the current C++ codebase.
The C++ broker, DataBlock, and hub::Producer/Consumer have superseded the async Python Hub.
