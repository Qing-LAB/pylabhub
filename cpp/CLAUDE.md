# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Configure (from cpp/ directory)
cmake -S . -B build                              # Default (Debug)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # Release
cmake -S . -B build -DPYLABHUB_USE_SANITIZER=Address   # ASan

# Build
cmake --build build
cmake --build build --target stage_all           # Stage all artifacts

# Run tests
ctest --test-dir build --rerun-failed --output-on-failure
ctest --test-dir build -R "^DataBlockTest"                             # Test suite
ctest --test-dir build -R "^FileLockTest.MultiProcessExclusiveAccess$" # Single test

# Format code
./tools/format.sh
```

Build outputs go to `build/stage-<buildtype>/` with `bin/`, `lib/`, `tests/`, `include/` subdirectories mirroring installation layout.

## Architecture

**pyLabHub C++** is a cross-platform IPC framework for scientific data acquisition. C++20, CMake 3.29+.

### Dual Library Structure

- **`pylabhub-basic`** (static, `pylabhub::basic`): Low-level, header-mostly utilities (in-process spinlock/SpinGuard, recursion_guard, scope_guard, platform detection). No external dependencies. Code here **cannot** depend on `pylabhub-utils`.
- **`pylabhub-utils`** (shared, `pylabhub::utils`): High-level utilities (Logger, FileLock, Lifecycle, JsonConfig, MessageHub, DataBlock). Depends on fmt, nlohmann_json, libzmq, libsodium, luajit.

Always link against alias targets: `pylabhub::utils`, not `pylabhub-utils`.

### Layered Umbrella Headers

Include one header per abstraction level — they handle all transitive includes:

- **Layer 0** `plh_platform.hpp` — Platform detection macros, version API
- **Layer 1** `plh_base.hpp` — Format tools, in-process spinlock (SpinGuard), recursion/scope guards, module definitions
- **Layer 2** `plh_service.hpp` — Lifecycle, FileLock, Logger
- **Layer 3** `plh_datahub.hpp` — JsonConfig, MessageHub, DataBlock

### Key Design Patterns

**Pimpl idiom** is mandatory for all public classes in `pylabhub-utils` (shared library ABI stability). Private members go in the `Impl` struct defined only in `.cpp`. The destructor must be defined in `.cpp`.

**Lifecycle management**: Utilities requiring init/shutdown register a `ModuleDef` via `GetLifecycleModule()`. In `main()`, `LifecycleGuard` handles topological-sort initialization and reverse-order teardown.

**DataBlock (shared memory IPC)**: Single shared memory segment with ring buffer of data slots. Two-tier synchronization: `DataBlockMutex` (OS-backed, for control zone) and `SharedSpinLock` (atomic PID-based, for data slots). Three API layers: Primitive (manual acquire/release), Transaction (scoped RAII wrapper), Script bindings (Python/Lua).

**Async Logger**: Command-queue architecture with dedicated worker thread. Lock-free enqueue from application threads; single-consumer worker handles I/O via pluggable sinks (console, file, rotating file, syslog, Windows event log).

### Test Organization

- `tests/test_pylabhub_corelib/` — Tests for `pylabhub-basic`
- `tests/test_pylabhub_utils/` — Tests for `pylabhub-utils`
- `tests/test_framework/` — Shared infrastructure (worker dispatchers for multi-process tests)

Uses GoogleTest. Multi-process IPC tests spawn child worker processes coordinated by parent.

## Project Rules

- **Do not modify** anything under `third_party/` unless explicitly instructed.
- Documentation in `docs/` is treated as code — update it when changing design patterns or behavior.
- HEP documents (`docs/HEP/`) are the authoritative design specifications for each component.
- Code style: `.clang-format` (LLVM-based, 4-space indent, 100-char lines, Allman braces). `.clang-tidy` runs automatically with Clang and treats warnings as errors.
- Cross-reference `CMakeLists.txt` files and `cmake/` helpers before proposing build changes.
