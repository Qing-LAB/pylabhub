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
- **Do NOT delete C API tests** (slot_rw_coordinator, recovery_api). These are the foundational
  test layer. Any reorganization must preserve and preferably extend C API coverage.
  See `docs/IMPLEMENTATION_GUIDANCE.md` § "C API Test Preservation".
- **Core structure changes** (`SharedMemoryHeader`, `DataBlockConfig`, BLDS schema, etc.) require
  the mandatory review checklist in `docs/IMPLEMENTATION_GUIDANCE.md` § "Core Structure Change Protocol"
  before any modification.
- **Error handling in broker/producer/consumer**: follow the two-category error taxonomy in
  `docs/IMPLEMENTATION_GUIDANCE.md` § "Error Taxonomy — Broker, Producer, and Consumer".

## TODO Bookkeeping (Mandatory)

**Every implementation session must maintain the TODO system.** Failure to do this is what
causes us to lose track of open items across sessions.

### Where to look first

Before starting any task, **always check**:
1. `docs/TODO_MASTER.md` — current sprint and area status; lists any active code reviews
2. `docs/todo/<area>_TODO.md` — detailed open items for the relevant area
3. `docs/code_review/` — any active code review file (`REVIEW_<Module>_YYYY-MM-DD.md`)
   with an ❌ OPEN status table. Multiple reviews may be active simultaneously.

### What to do when completing work

After implementing anything non-trivial, **before responding "done"**:

1. **Update the subtopic TODO** (`docs/todo/*.md`):
   - Completed items → move to "Recent Completions" with date + one-line description
   - Newly discovered items → add to "Current Focus" with file reference

2. **Update `docs/TODO_MASTER.md`** status table if the area's state changed;
   update or remove any `Active code review:` reference when all items are resolved

3. **Update any active code review** in `docs/code_review/` whose items were addressed:
   mark ✅ FIXED with date. Reviews follow the naming pattern `REVIEW_<Module>_YYYY-MM-DD.md`.
   When all items are ✅, archive per `docs/DOC_STRUCTURE.md §2.2`.

4. **Never leave an open item only in chat history or an inline `// TODO:` comment** —
   it must be in a subtopic TODO to survive context resets

5. **Transient documents** (design notes, plans, analyses, audits) must be placed in the
   correct location per `docs/DOC_STRUCTURE.md`:
   - Active design/implementation drafts → `docs/tech_draft/`
   - In-progress module-specific code reviews → `docs/code_review/`
   - Do NOT create free-floating `.md` files directly under `docs/` root
   - Any open item in a transient document must also appear in the relevant subtopic TODO
   - When the work captured in a transient document is complete and verified against code:
     **merge** any lasting insight into core docs, then **archive** the transient doc to
     `docs/archive/transient-YYYY-MM-DD/` and record it in `docs/DOC_ARCHIVE_LOG.md`

### Canonical subtopic TODOs

| Area | File |
|---|---|
| RAII layer / `with_transaction` / Phase 3 | `docs/todo/RAII_LAYER_TODO.md` |
| Primitive API / ABI / concurrency / lifecycle | `docs/todo/API_TODO.md` |
| Windows/MSVC / cross-platform / CMake flags | `docs/todo/PLATFORM_TODO.md` |
| Tests / coverage / new test scenarios | `docs/todo/TESTING_TODO.md` |
| Memory layout / `SharedMemoryHeader` / struct sizes | `docs/todo/MEMORY_LAYOUT_TODO.md` |
| MessageHub / broker protocol | `docs/todo/MESSAGEHUB_TODO.md` |

Full rules in `docs/IMPLEMENTATION_GUIDANCE.md` § "Session Hygiene"
and `docs/DOC_STRUCTURE.md` §1.7 (code review lifecycle).
