# HEP-CORE-0032: ABI Compatibility Design

| Property       | Value                                                          |
|----------------|----------------------------------------------------------------|
| **HEP**        | `HEP-CORE-0032`                                                |
| **Title**      | ABI Compatibility Design — Export Policy, Binding Surface, and Stability |
| **Status**     | Draft (2026-04-16)                                             |
| **Created**    | 2026-04-16                                                     |
| **Area**       | Public API (`pylabhub-utils`), All Consumers                   |
| **Depends on** | None — cross-cutting policy                                    |

---

## 1. Motivation

`pylabhub-utils` is a shared library (`.so` / `.dll`) consumed in three ways:

1. **C++ programs with headers** — compile against public headers, link against
   the lib. Same stdlib assumed.
2. **Language bindings** (Python via pybind11, future FFI) — call into the lib
   through a binding layer. No C++ headers at the binding target.
3. **C programs** — use the C API subset (`slot_rw_coordinator.h`,
   `recovery_api.hpp`, `native_engine_api.h`). Full cross-compiler ABI.

The library currently exports ~870 C++ symbols. Most are internal implementation
details that no external consumer needs. Several use types (`std::function`,
`std::filesystem::path`, `std::optional`) that are not ABI-stable across
compiler or stdlib versions. This creates fragility when the library is consumed
by code compiled with a different toolchain.

This HEP establishes the export policy, categorises every exported class, and
defines the mechanisms for controlling symbol visibility.

---

## 2. Two-Tier API Model

### Tier 1: C Binding Surface (ABI-stable)

Flat `extern "C"` functions with C-compatible types only:
- `const char*` for strings
- C function pointers (with `void* ctx`) for callbacks
- POD structs for aggregate data
- Primitives (`int`, `uint64_t`, `bool`, `size_t`)
- Opaque handles (`void*` or typed opaque pointers)

**Target consumers:** Python bindings, FFI from any language, C programs,
programs compiled with a different compiler or stdlib version.

**Existing C API surface:** `slot_rw_coordinator.h` (14 functions),
`recovery_api.hpp` (13 functions), `crypto_utils.hpp` (5 functions),
`native_engine_api.h` (plugin interface), `platform` functions (12 functions).

**Future additions:** `plh_role_init()`, `plh_role_run()`, and other
flat helper functions that compose the C++ internals for binding use
(see HEP-CORE-0024 §11).

### Tier 2: C++ Header API (same-stdlib)

C++ classes with pimpl, exported via `PYLABHUB_UTILS_EXPORT`. Consumers
include the public headers and link against the lib. The caller and library
must use the **same stdlib implementation** (e.g., both libstdc++).

**Key guarantee:** pimpl ensures the class layout is a single pointer.
Adding/removing private members does not break ABI. Only method signature
changes (parameter types, return types) affect compatibility.

**Safe types in Tier 2 method signatures:**

| Type | Why safe |
|------|----------|
| `std::string` | ABI-stable on libstdc++ since GCC 5 (SSO layout). Passed by `const&` or value. |
| `std::string_view` | Trivial layout (`const char*` + `size_t`). |
| `std::unique_ptr<T>` | Single pointer with default deleter. Standard pimpl return type. |
| `nlohmann::json` | Header-only lib — same header = same layout. Distributed with the project. |
| Enums, primitives, `const char*` | C-compatible. Always safe. |
| `std::chrono::duration`, `time_point` | Fixed layout (single integer). |
| `std::array<T,N>` | Fixed layout. |

**Unsafe types — avoid in Tier 2 public signatures:**

| Type | Why unsafe |
|------|------------|
| `std::function` | Layout varies across stdlib versions. No ABI guarantee. |
| `std::filesystem::path` | Layout changed between libstdc++ versions (GCC 8→12). |
| `std::optional<T>` | Layout is implementation-defined (flag + storage). |
| `std::vector<T>` | Layout is three pointers — stable in practice but technically impl-defined. Use sparingly; prefer returning via output parameter or JSON. |
| `std::unordered_map` | Complex internal layout. Never in public signatures. |
| `std::shared_ptr<T>` | Two pointers + control block. Cross-module sharing is fragile. |

---

## 3. Export Macro Categories

### 3.1 `PYLABHUB_UTILS_EXPORT`

Full production export. The symbol is visible in all builds (debug, release,
test). Use for classes and functions that external C++ consumers need.

**Criteria:** The class is constructed, called, or subclassed by code outside
the shared library (external binaries, plugins, user C++ programs).

### 3.2 `PYLABHUB_UTILS_TEST_EXPORT`

Test-only export. The symbol is visible only when `PYLABHUB_BUILD_TESTS` is
defined (i.e., test builds). In production builds the symbol is hidden.

**Criteria:** The class is internal implementation — used only within the lib
and by test binaries that need direct access for unit testing.

**Definition** (appended to the generated `pylabhub_utils_export.h` via
CMake `CUSTOM_CONTENT_FROM_VARIABLE`):

```c
#ifdef PYLABHUB_BUILD_TESTS
#   define PYLABHUB_UTILS_TEST_EXPORT PYLABHUB_UTILS_EXPORT
#else
#   define PYLABHUB_UTILS_TEST_EXPORT
#endif
```

### 3.3 No macro (hidden)

The symbol is never exported. Internal functions and classes that are only
called from within the same translation unit or within the shared lib.
Header-only utilities, anonymous-namespace helpers, and `static` functions
fall here automatically.

---

## 4. Class Classification

### 4.1 Production Export (`PYLABHUB_UTILS_EXPORT`)

Classes that external C++ consumers construct or call through:

| Class | Pimpl | Notes |
|-------|-------|-------|
| `RoleAPIBase` | Yes | Central role API. External users build roles via this. |
| `JsonConfig` | Yes | Config file management. |
| `Logger` | Yes | Logging framework. |
| `LifecycleManager` | Yes | Module init/shutdown. |
| `ModuleDef` | Yes | Lifecycle module definition. |
| `FileLock` | Yes | Cross-process file locking. |
| `RoleDirectory` | No* | Path resolution. *Needs pimpl — stores `path` directly. |
| `HubVault` | Yes | Hub keypair management. |
| `RoleVault` | Yes | Role keypair management. |
| `DataBlockProducer` | Yes | SHM producer handle. |
| `DataBlockConsumer` | Yes | SHM consumer handle. |
| `DataBlockMutex` | No | SHM mutex (fixed layout, `alignas`). |
| `DataBlockLockGuard` | No | RAII guard (holds reference only). |
| `SharedSpinLock` | No | Atomic spinlock (fixed layout, `alignas`). |
| `SharedSpinLockGuard` | No | RAII guard. |
| `QueueReader` | No | Abstract interface (virtual). |
| `QueueWriter` | No | Abstract interface (virtual). |
| `BrokerService` | Yes | Hub broker (hubshell uses it). |
| `BrokerRequestComm` | Yes | Broker client communication. |
| `HubConfig` | Yes | Hub configuration. |
| `InteractiveSignalHandler` | Yes | Signal handling for CLI binaries. |
| `ThreadManager` | Yes | Thread lifecycle management. |
| `RoleConfig` | Yes | Role configuration loading. |
| `ProducerRoleHost` | No* | *Currently needs export for separate `main.cpp` binaries. Will become test-only after binary unification. |
| `ConsumerRoleHost` | No* | Same as above. |
| `ProcessorRoleHost` | No* | Same as above. |

### 4.2 Test-Only Export (`PYLABHUB_UTILS_TEST_EXPORT`)

Internal classes that only tests access directly:

| Class | Reason tests need it |
|-------|---------------------|
| `RoleHostCore` | L2 unit tests construct it directly for metrics/shutdown testing. |
| `NativeEngine` | L2 engine tests instantiate it directly. |
| `SchemaLibrary` | L3 schema tests. |
| `SchemaStore` | L3 schema tests. |
| `ContextMetrics` | L2/L3 queue metric tests. |
| `SlotWriteHandle` | L3 DataBlock tests. |
| `SlotConsumeHandle` | L3 DataBlock tests. |
| `SlotRecovery` | L2 recovery tests. |
| `SlotDiagnostics` | L2 diagnostic tests. |
| `IntegrityValidator` | L2 integrity tests. |
| `HeartbeatManager` | L2 heartbeat tests. |

### 4.3 Hidden (no export needed)

Internal classes that are never accessed from outside the lib:

| Class/Function | Why hidden |
|----------------|-----------|
| `RoleInitEntry` | Internal to `role_directory.cpp`. |
| `CycleOps` classes | Internal to `cycle_ops.hpp`, template-instantiated. |
| `run_data_loop` | Template in internal header. |
| `retry_acquire` | Compiled in `data_loop.cpp`, called only by CycleOps (in lib). |
| `AcquireContext`, `LoopConfig` | Internal structs used by data loop. |

---

## 5. Signature Audit — Unsafe Types in Production Exports

### 5.1 `std::function` (15 signatures, 9 classes)

| Class | Method | Caller | Action |
|-------|--------|--------|--------|
| `RoleRegistrationBuilder` | `config_template()`, `on_init()` | External | **Fix: use function pointers** |
| `RoleAPIBase` | `set_metrics_hook()` | External | **Fix: use function pointer** |
| `BrokerRequestComm` | `on_hub_dead()`, `on_notification()`, `run_poll_loop()`, `set_periodic_task()` | Internal (lib) | Low risk — same binary |
| `ThreadManager` | `spawn()` | Internal (lib) | Low risk — same binary |
| `RoleHostCore` | `open_inbox()` | Internal (lib) | Low risk — same binary |
| `RoleConfig` | `load()`, `load_from_directory()` | `main()` | Low risk — same binary |
| `InteractiveSignalHandler` | `set_status_callback()` | `main()` | Low risk — same binary |
| `LifecycleManager` | `set_lifecycle_log_sink()` | `main()` | Low risk — same binary |
| `Logger` | `set_write_error_callback()` | `main()` | Low risk — same binary |

**Immediate action:** Fix the 3 external-facing signatures (builder + metrics hook).

### 5.2 `std::filesystem::path` (31 signatures, 10 classes)

Pervasive. Used as `const path&` parameters in constructors and factory
methods of `FileLock`, `JsonConfig`, `Logger`, `RoleDirectory`, vaults,
`HubConfig`.

**Impact of fixing:** All 10 classes would need `const char*` overloads or
complete C API wrappers. Large scope.

**Decision:** Defer to the C API layer (Tier 1). Tier 2 consumers (C++ with
same stdlib) are not affected. Document as a known limitation.

### 5.3 `std::optional` (11 signatures, 6 classes)

Most uses are internal. The 4 `RoleAPIBase` methods use
`std::optional<ChannelSide>` which could be replaced with a defaulted enum
value (`ChannelSide::Auto`).

**Decision:** Fix `RoleAPIBase` signatures (small change). Leave internal
uses as-is.

---

## 6. Implementation Plan

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | Add `PYLABHUB_UTILS_TEST_EXPORT` via `CUSTOM_CONTENT_FROM_VARIABLE` | ⚪ |
| 2 | Apply `TEST_EXPORT` to internal-only classes (§4.2) | ⚪ |
| 3 | Fix `std::function` in external-facing signatures (§5.1, 3 methods) | ⚪ |
| 4 | Fix `std::optional<ChannelSide>` in RoleAPIBase (§5.3, 4 methods) | ⚪ |
| 5 | After binary unification: move RoleHosts to `TEST_EXPORT` | ⚪ |
| 6 | C API helper functions for Tier 1 binding surface | ⚪ |
| 7 | `std::filesystem::path` → `const char*` in C API layer | ⚪ |

---

## 7. Guidelines for New Code

1. **New public classes MUST use pimpl.** (Existing mandate in CLAUDE.md.)
2. **No `std::function` in public method signatures.** Use C function pointers
   with `void* ctx` if callbacks are needed at the API boundary.
3. **No `std::filesystem::path` in new public method signatures.** Use
   `const char*` or `std::string_view`. Convert to `path` internally.
4. **No `std::optional` in new public method signatures.** Use sentinel values
   or pointer parameters for optional data.
5. **Internal classes use `PYLABHUB_UTILS_TEST_EXPORT`**, not
   `PYLABHUB_UTILS_EXPORT`.
6. **Evaluate whether a class needs export at all.** If only called within the
   lib, it should be hidden (no macro).

---

## 8. Copyright

Copyright (c) 2026 pyLabHub Contributors. All rights reserved.
