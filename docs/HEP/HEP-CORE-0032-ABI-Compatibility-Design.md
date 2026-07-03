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

## 8. Handshake ABI Fingerprint

**Amended 2026-07-03.**  §§1–7 above cover API-surface stability at
the SO / .dll level (what a linker sees).  §8 covers a distinct
concern: how peers on the wire prove they were built from a
compatible ABI *before* they exchange trust-anchored data.

**Composes with existing infrastructure.**  This section does NOT
introduce new versioning axes.  It wire-binds the existing
`pylabhub::version::ComponentVersions` (7 axes with per-axis
major/minor) + `build_id` from `plh_version_registry.hpp`, and
routes REG_REQ / CONSUMER_ATTACH_REQ ingest through the existing
`pylabhub::version::check_abi()` verdict function.  See
HEP-CORE-0026 for the registry design; see `plh_version_registry.hpp`
§ "Versioning axes" table for the canonical per-axis bump rules.

### 8.1 Motivation

`validate_header_layout_hash` (existing, called by `DataBlockConsumer`
attach at `data_block.cpp:3107` and by the observer factory added
for task #317) is a per-attach ABI gate: it catches a broker +
producer built from divergent `SharedMemoryHeader` layouts at the
moment one process tries to mmap the other's segment.  Two gaps:

1. **Too late in the flow.**  Discovery happens after registration,
   after credentials are exchanged, after the consumer has spent
   budget dialling.  A broker-side observability channel that
   catches incompatibility at REG_REQ ingest is cheaper and
   centralises the failure signal.

2. **Header-only.**  `SharedMemoryHeader` covers ONE of the seven
   ABI axes (`shm`).  The other six (`library`, `broker_proto`,
   `zmq_frame`, `script_api`, `script_engine`, `config`) drift
   independently.  `check_abi()` already exists and compares all
   seven — but only in-process at startup, not on the wire between
   broker and role.

**Solution:** every REG_REQ (producer + consumer) and every
CONSUMER_ATTACH_REQ (HEP-CORE-0041 SHM handshake) carries the full
`ComponentVersions` envelope + `build_id`.  Broker feeds the received
envelope into the existing `check_abi()` and logs the verdict;
broker echoes its own envelope back so role sees broker's build.
Central observability, per-axis discrimination in the log line, opt-in
strict enforcement.

### 8.2 Wire envelope

Wire field name: `abi_fingerprint`.  Shape mirrors
`pylabhub::version::version_info_json()` — the canonical JSON form
already exposed via the C-linkage symbol `pylabhub_abi_info_json()`:

```
"abi_fingerprint": {
  "release":              "<PEP 440 string>",
  "library":              "<major>.<minor>.<rolling>",
  "shm_major":            <uint16>,  "shm_minor":            <uint16>,
  "broker_proto_major":   <uint16>,  "broker_proto_minor":   <uint16>,
  "zmq_frame_major":      <uint16>,  "zmq_frame_minor":      <uint16>,
  "script_api_major":     <uint16>,  "script_api_minor":     <uint16>,
  "script_engine_major":  <uint16>,  "script_engine_minor":  <uint16>,
  "config_major":         <uint16>,  "config_minor":         <uint16>,
  "build_id":             "<opt string, present iff PYLABHUB_HAVE_BUILD_ID>"
}
```

Carried on:
- **REG_REQ** (both producer and consumer variants).
- **CONSUMER_ATTACH_REQ** (HEP-CORE-0041 §9 D4).

Broker's response carries the broker's own envelope under a distinct
wire field name to keep serialisation straightforward:
- **REG_ACK** (both producer + consumer): `broker_abi_fingerprint`.
- **CONSUMER_ATTACH_ACK**: `broker_abi_fingerprint`.

### 8.3 Versioning taxonomy — MAJOR / MINOR / BUILD

The MAJOR/MINOR bump rules are documented per-axis in
`plh_version_registry.hpp` § "Bump rules (same for every axis)"
and § "Per-axis bump history."  §8.3 tabulates them here for the
handshake context, adds a BUILD-only column for concerns that
change bytes without triggering either MAJOR or MINOR, and pins the
log-format stability contract that L3 tests depend on.

#### 8.3.1 MAJOR bump — reject on mismatch (strict mode)

Per `plh_version_registry.hpp`: "removed/renamed field or method,
changed semantics, changed layout or encoding.  Caller with
mismatched major must reject at the boundary."  Concrete examples
per axis (indicative, not exhaustive):

| Axis | MAJOR-worthy change | Rationale |
|---|---|---|
| `library` | ABI break in an exported symbol (see §§3-4 above) | Consumers linked against old ABI misinterpret. |
| `shm` | `SharedMemoryHeader` layout add/remove/reorder/resize | Peer reads at wrong offset. |
| `shm` | `SharedMemoryHeader` semantic change on existing field | Peer reads correct offset but misinterprets. |
| `broker_proto` | Rename/retype/remove existing REG_REQ field; retire message type | Serializer/deserializer breaks. |
| `broker_proto` | `protocol_version` field bump (existing HEP-CORE-0036) | Same. |
| `zmq_frame` | Change `[magic, tag, seq, payload, checksum]` framing shape | Parser breaks. |
| `script_api` | Remove Python/Lua binding entry point | Scripts break. |
| `script_engine` | Change `ScriptEngine` virtual interface signature | Plugin ABI break. |
| `config` | Remove or rename a required config key | Existing config files break. |

#### 8.3.2 MINOR bump — warn + accept

Per `plh_version_registry.hpp`: "new optional field, new method with
base default.  Caller with lower minor logs WARN and proceeds."

| Axis | MINOR-worthy change | Rationale |
|---|---|---|
| `library` | Add new exported class / function | Callers that don't use it are unaffected. |
| `shm` | Add optional trailing header field within a reserved region | Old peers use reserved zero. |
| `broker_proto` | Add new optional field on existing message; add new message type; add new enum value | JSON: unknown keys ignored.  New message: older peer replies UNKNOWN_TYPE. |
| `broker_proto` | Add new metric field in HEP-CORE-0019 metrics tree | Metrics is additive by contract. |
| `zmq_frame` | Add new tag value | Unknown tag → safe fallback per HEP-CORE-0007. |
| `script_api` | Add new binding entry point | Old scripts unaffected. |
| `script_engine` | Add new optional callback with base default | Plugin unaffected. |
| `config` | Add new optional config key with default | Old configs still parse. |
| **any axis** | **Log message CONTENT change** | Tests read message content — content-shape stability is a MINOR-bump contract. |

#### 8.3.3 BUILD-only — no axis bump (build_id changes only)

Everything that changes bytes without changing behavior contracts.
None of these require bumping ANY of the seven axes; they only
change `PYLABHUB_BUILD_ID` (git commit hash) and are surfaced on the
wire via the `build_id` field.

| Change | Rationale |
|---|---|
| Compile flags: debug vs release, sanitizers on/off, optimization level | No behavior contract change. |
| Default log level | Log level is operator concern, not peer concern. |
| Log timestamp precision, log line framing (level prefix, thread-id) | NOT log message content (which is MINOR). |
| Config default values that aren't enforced by protocol | Role can override. |
| Third-party lib version bumps with unchanged API | No observable difference. |
| Non-inlined tuning constants within HEP-declared budgets | Same. |

### 8.4 Log-format stability contract

**Log message CONTENT is a MINOR-bump concern.**  L2/L3 tests
routinely assert `EXPECT_LOG_CONTAINS("substring")` — the substring is
part of the message content.  Reshaping a log message so a test's
substring no longer matches IS a MINOR-observable change and MUST
bump a MINOR on the appropriate axis (typically the axis whose
subsystem owns the log line — e.g. broker_proto for broker log
messages, script_api for script-side log messages).

**Log framing (timestamp, level, thread-id prefix) is BUILD-only.**
Tests must not assert against framing — a test that does is
mis-scoped and should be corrected.

### 8.5 Behavioral policy — reuse `check_abi()`

Verdict is derived from the existing `pylabhub::version::check_abi()`
result (`AbiCheckResult`):
- `r.compatible` — no MAJOR mismatch, `build_id` matches (when both
  sides provide one).
- `r.major_mismatch.<axis>` — per-axis MAJOR mismatch flag.
- Minor mismatches are logged to stderr WARN inside `check_abi()`
  itself; §8.6 also captures them in the broker's structured log.

| Comparison | Broker default | Broker strict | Role default | Role strict |
|---|---|---|---|---|
| envelopes match | OK — INFO, accept | OK — INFO, accept | OK — INFO, accept | OK — INFO, accept |
| `build_id` differs only | INFO, accept | INFO, accept | INFO, accept | INFO, accept |
| minor on ≥1 axis, no major | WARN, accept | WARN, accept | WARN, accept | WARN, accept |
| major on ≥1 axis | WARN, accept | ERROR, reject REG_ACK | WARN, accept | ERROR, refuse Registered |

Strict mode is opt-in via configuration:
- **`broker.strict_abi_mismatch`** (bool, default `false`)
- **`role.strict_abi_mismatch`** (bool, default `false`) — role verifies broker's envelope in REG_ACK.

Rationale for default `false`: rolling upgrades of a fleet should not
break connectivity mid-rollout; operators enable strict mode for
regulated / production-hardened deployments where a build-version
mismatch is a runbook-worthy event.

### 8.6 Log format (canonical)

Log lines follow **Convention 1** (task #238 style,
`[component] event=EventName field='value'`) — the same convention
already used on the sibling REG_REQ log line at
`broker_service.cpp:2342` (`[broker] event=RegReqAccepted role='X'
channel='Y' producer_pubkey='Z'`).  Deviating from this convention
without a `broker_proto` MINOR bump per §8.4 breaks the
test-contract discipline established by task #238.

**Verdict line (always logged on REG_REQ ingest):**

```
[broker] event=AbiFingerprintReceived role='{X}' verdict='{V}' mismatched_axes='{list}'
```

Where:
- `{X}` = role's `role_uid`, single-quoted.
- `{V}` ∈ `OK` | `BUILD_ONLY` | `MINOR_MISMATCH` | `MAJOR_MISMATCH_ACCEPTED` | `MAJOR_MISMATCH_REJECTED` | `ABSENT`, single-quoted.
- `{list}` = comma-separated axis names (e.g. `shm,broker_proto`) or empty on OK / BUILD_ONLY / ABSENT.  Single-quoted.

Verdict values (matches §8.5 table):
- `OK` — envelopes match.
- `BUILD_ONLY` — only `build_id` differs.
- `MINOR_MISMATCH` — one or more axes differ on minor only.
- `MAJOR_MISMATCH_ACCEPTED` — major mismatch, default (lenient) mode.
- `MAJOR_MISMATCH_REJECTED` — major mismatch, strict mode.
- `ABSENT` — role's REG_REQ carried no `abi_fingerprint` (migration
  window while roles roll out §8's wire binding).

**Detail line (logged at same INFO level, only when verdict ≠ OK):**

```
[broker] event=AbiFingerprintDetail role='{X}' role_versions='{s}' broker_versions='{s}'
```

Where `{s}` = existing `pylabhub::version::version_info_string()`
output.  The `version_info_string()` output shape is stable per
`plh_version_registry.hpp` and MUST NOT be refactored without a
`library` axis MINOR bump.

**Symmetric role-side lines** on REG_ACK receive (broker's echo
carries `broker_abi_fingerprint`):

```
[role] event=BrokerAbiFingerprintReceived role='{X}' verdict='{V}' mismatched_axes='{list}'
[role] event=BrokerAbiFingerprintDetail role='{X}' role_versions='{s}' broker_versions='{s}'
```

**Same treatment on CONSUMER_ATTACH_REQ_SHM / _ZMQ ingest** (HEP-0041
§9 D4, HEP-0042 §6.2):

```
[broker] event=ConsumerAttachAbiReceived role='{X}' transport='{shm|zmq}' verdict='{V}' mismatched_axes='{list}'
```

**These log-line templates are test-contract-stable**, pinned by L3
tests (task #326).  Refactoring them requires a MINOR bump on the
appropriate axis per §8.4.  New sites emitting the fingerprint
verdict MUST follow this Convention 1 shape — grep neighbours in the
same file (`broker_service.cpp` sibling REG_REQ lines) for pattern
parity before writing new log calls.

### 8.7 Handshake sequence

```mermaid
sequenceDiagram
    participant Role
    participant Broker

    Role->>Broker: REG_REQ + abi_fingerprint (ComponentVersions + build_id)
    Note over Broker: run check_abi(role_versions, role_build_id)<br/>log per §8.6

    alt any major mismatch AND broker.strict_abi_mismatch=true
        Broker-->>Role: REG_ACK {status: "abi_major_mismatch", mismatched_axes: [...]}
        Note over Role: role bails; no Registered transition
    else all other cases
        Broker-->>Role: REG_ACK {status: "ok", broker_abi_fingerprint}
        Note over Role: run check_abi(broker_versions, broker_build_id)<br/>if role.strict_abi_mismatch=true AND any major → bail
        Note over Role: else → Registered
    end
```

Same envelope + verification applies to CONSUMER_ATTACH_REQ /
CONSUMER_ATTACH_ACK on the HEP-CORE-0041 SHM handshake path.

### 8.8 No new constants needed

All source-of-truth lives in `plh_version_registry.hpp`:
- Per-axis `inline constexpr uint16_t k<Axis>Major / k<Axis>Minor`.
- `ComponentVersions::current()` returns the local library's view.
- `version_info_json()` produces the canonical JSON envelope for §8.2.
- `check_abi(expected, build_id)` produces the verdict for §8.5.
- `PYLABHUB_BUILD_ID` (`pylabhub_build_id.h`, conditionally-generated
  when `PYLABHUB_HAVE_BUILD_ID` is defined) provides the build hash
  for §8.3.3.

Bumping a MAJOR or MINOR is done by editing the `k<Axis>Major` /
`k<Axis>Minor` constant in `plh_version_registry.hpp` in the same PR
as the change per §8.3.  Drift-guard `static_assert`s in
`version_registry.cpp` catch un-mirrored C-visible constant
divergence at compile time.

### 8.9 Cross-references

- **HEP-CORE-0026** (Version Registry) — the source-of-truth HEP for
  `ComponentVersions` + `check_abi()`.  §8 wire-binds that registry.
- **HEP-CORE-0028** (Native Plugin C ABI) — the `PLH_COMPONENT_*`
  C-visible mirrors used by native plugins live there; drift is
  static-asserted in `version_registry.cpp:36-59`.
- **HEP-CORE-0036 §5b** (canonical wire schema) — wire-schema changes
  classify against §8.3 to determine bump axis + level.
- **HEP-CORE-0041 §9 D4** — CONSUMER_ATTACH_REQ carries the same envelope.
- **HEP-CORE-0041 §10.5** (broker SHM observation) — observer attach
  fingerprint is echoed on CONSUMER_ATTACH_ACK.
- **`validate_header_layout_hash`** — remains the per-attach in-process
  gate for the `shm` axis specifically.  §8's wire fingerprint is the
  earlier + centralised + multi-axis counterpart.

### 8.10 Implementation tracking

- **#324** — this design amendment (in this HEP).
- **#325** — impl: emit ComponentVersions envelope on REG_REQ + CONSUMER_ATTACH_REQ; broker calls `check_abi()`; both sides log per §8.6; strict-mode config knobs.
- **#326** — L2/L3 tests: matched / minor / major (lenient) / major (strict) / build-only-diff paths.

---

## 9. Copyright

Copyright (c) 2026 pyLabHub Contributors. All rights reserved.
