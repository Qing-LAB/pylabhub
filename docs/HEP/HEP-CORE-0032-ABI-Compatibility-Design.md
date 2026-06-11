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

Test-only export.  The symbol is visible when the CMake option
`-DBUILD_TESTS=ON` is set (which compiles `PYLABHUB_BUILD_TESTS`),
otherwise hidden.  This is **visibility-only** — it widens the ABI
surface for test binaries to instantiate framework-internal classes
directly, but it is not itself a security boundary.  Genuine
security-sensitive backdoors (counter mutators, auth bypass) use a
second source-level `!defined(NDEBUG)` gate on top of
`PYLABHUB_BUILD_TESTS` (see §3.2.1 "Two risk classes" below).

**Criteria:** The class is internal implementation — used only within the lib
and by test binaries that need direct access for unit testing.

**Definition** (appended to the generated `pylabhub_utils_export.h` via
CMake `CUSTOM_CONTENT_FROM_VARIABLE`):

```c
/* Test-only export: visible when PYLABHUB_BUILD_TESTS is defined,
   hidden otherwise.  Visibility-only — not a security gate.  For
   genuine security-sensitive backdoors (counter mutators, auth
   bypass), use the additional `!defined(NDEBUG)` source-level gate.
   See HEP-CORE-0032 §3.2.1. */
#ifdef PYLABHUB_BUILD_TESTS
#   define PYLABHUB_UTILS_TEST_EXPORT PYLABHUB_UTILS_EXPORT
#else
#   define PYLABHUB_UTILS_TEST_EXPORT
#endif
```

#### 3.2.1 Deployment posture and two risk classes (#205, 2026-06-11)

`BUILD_TESTS=ON` is the default in both Debug and Release.  Tests for
the normal public API build and run in both configurations.

The codebase distinguishes two risk classes of test-only surface:

| Risk class | Examples | Gate | Behaviour in Release+BUILD_TESTS=ON |
|---|---|---|---|
| **(A) Test-only visibility — diagnostic / recovery classes** | `IntegrityValidator`, `SlotDiagnostics`, `SlotRecovery`, `HeartbeatManager`, `verify_ownership` | `defined(PYLABHUB_BUILD_TESTS)` (via `PYLABHUB_UTILS_TEST_EXPORT`) | Present, exported. ABI surface is wider than a shipped binary needs, but not a security risk per se. |
| **(B) Genuine security backdoor — counter forgery, auth bypass, invariant relaxation** | `RoleHostCore::test_set_*` counter mutators; HEP-CORE-0036 §I10 one-pubkey-per-role-uid bypass | `defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)` (or, for I10, `defined(PYLABHUB_WITH_TEST) && !defined(NDEBUG)`) | **Absent** at the source level.  Counter forgery and invariant relaxation are physically impossible regardless of how `BUILD_TESTS` is configured. |

Tests that depend on class (B) backdoors skip gracefully in Release:

| Configuration | Class A in binary | Class B in binary | Normal-API tests | Class A tests | Class B tests |
|---|---|---|---|---|---|
| **Debug** + `BUILD_TESTS=ON` (the dev config) | Present | Present | Build + run | Build + run | Build + run (real path) |
| **Release** + `BUILD_TESTS=ON` | Present | **Absent** | Build + run | Build + run | Build, skip via `GTEST_SKIP()` |
| Any + `BUILD_TESTS=OFF` (the ship config) | Absent | Absent | Not built | Not built | Not built |

The class (B) promise is enforced at three layers:

1. **Source-level gate on the backdoor itself** —
   `RoleHostCore::test_set_*` and the I10 bypass branch are wrapped in
   `#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)`.  Result:
   Release binaries do not include the surface even when
   `BUILD_TESTS=ON`.

2. **Source-level wrap on backdoor CALL SITES** — every test/worker
   file that invokes a class (B) backdoor wraps the call in
   `#if !defined(NDEBUG)` so the file compiles in Release.

3. **TEST_F-level `GTEST_SKIP()` in NDEBUG** — each `TEST_F` body that
   depends on backdoor behavior starts with:
   ```cpp
   #if defined(NDEBUG)
       GTEST_SKIP() << "Requires <backdoor API>; absent in Release "
                       "(HEP-CORE-0032 §3.2).";
   #else
       ... real test body ...
   #endif
   ```
   so CI in Release reports the test as SKIPPED (not failed, not built
   wrong).

This applies to **every** class (B) test-only relaxation added to the
codebase.  HEP-CORE-0036 §I10 is the canonical security example (the
one-pubkey-per-role-uid invariant uses `PYLABHUB_WITH_TEST` rather than
`PYLABHUB_BUILD_TESTS`, but the gating shape is identical:
`#if defined(NDEBUG) || !defined(PYLABHUB_WITH_TEST)` selects the
production-enforce branch).  The L2 test
`test_layer2_known_roles.cpp::I10_BuildFlag_MatchesNDebugDisposition`
is the gold-standard pin for that gate.  The build-flag invariants
test at `test_layer2_build_flag_invariants.cpp` extends the same
pattern to `RoleHostCore::test_set_*`.

The `PYLABHUB_VAULT_TEST_KDF` macro (`tests/CMakeLists.txt`) is a
separate, narrower gate — KDF parameters relaxed under
`BUILD_TESTS=ON AND CI-env-detected`.  It is acceptable because
production builds (`BUILD_TESTS=OFF`) cannot reach the relaxed branch;
its threat surface is contained to a single derivation function used
only by L4 vault tests.

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
| `BrokerService` | Yes | Hub broker (consumed by future `plh_hub` binary — HEP-CORE-0033 §15 Phase 9). |
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
| `SchemaLibrary` | L3 schema tests (stateless file loader; HEP-0034 §4). |
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
