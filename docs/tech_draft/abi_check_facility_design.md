# ABI-Check Facility Design

**Status:** draft
**Origin:** session 2026-04-21 (post-`ProcessorCliTest.Validate_ExitZero` SIGSEGV incident)
**Related:** HEP-CORE-0026 (version registry), HEP-CORE-0032 (ABI compatibility phases), HEP-CORE-0024 Phase 19-20 (plh_role binary unification)
**Tracked:** `docs/todo/API_TODO.md` — "ABI Check Facility (HEP-CORE-0026 extension)"

---

## 1. Motivation

### 1.1 The incident that surfaced this gap

During session 2026-04-21, after adding a new virtual method to `ScriptEngine`
(`pending_script_engine_request_count()`), the test `ProcessorCliTest.Validate_ExitZero`
began segfaulting (exit 139). Root cause: the `pylabhub-processor` binary was
compiled on 2026-04-20 against the prior `ScriptEngine` vtable layout;
`pylabhub-utils` (shared library) was rebuilt 2026-04-21 with the new layout.
The linker did not reject the combination because `SOVERSION` had not been
bumped — vtable ordering is not a structural ABI change the toolchain sees.
At runtime, `host.startup_()` called a virtual method via an offset valid
under the old vtable; that offset resolved to a different entry in the new
vtable, and the process died inside Python engine initialization.

This is a silent failure mode. The developer did not know the binary was stale;
the CI only saw a segfault without diagnostic.

### 1.2 What we want

A facility where:

- Each binary records, at compile time, the set of interface versions it was
  built against.
- At process startup, the binary compares its recorded expectations against
  the versions the linked library reports at runtime.
- A mismatch produces a clear, actionable diagnostic and refuses to run
  (or degrades gracefully on minor-only deltas).

## 2. Existing Infrastructure (HEP-CORE-0026)

Already in place in `plh_version_registry.hpp` + `version_registry.cpp`:

- **`ComponentVersions` struct** tracking four independent axes with explicit
  semantic-versioning discipline: `library`, `shm`, `wire`, `script_api`.
- **Query surface**: `pylabhub::version::current()`, `version_info_string()`,
  `version_info_json()`, plus `extern "C" pylabhub_abi_info_json(void)` as a
  dlsym-stable symbol.
- **ELF SONAME wiring** on `pylabhub-utils` (`src/utils/CMakeLists.txt:117-118`):
  `VERSION=<full>`, `SOVERSION=<major>`. Catches install-level major mismatches
  at dynamic-link time.
- **Native plugin ABI pattern** (`native_engine.cpp:869-878`): host loads
  plugin's `native_abi_info()` function, validates struct size + version,
  WARNs when absent.
- **Semantic rules (documented)**: major mismatch → incompatible (reject at
  boundary); minor mismatch → additive feature gap (log warning, proceed).

## 3. Gaps This Design Closes

1. **No "check against expected" API.** The facility is read-only. Consumers
   query versions but cannot assert compatibility.
2. **No `script_engine` axis.** Today's incident came from bumping a C++ virtual
   interface that is not represented in `ComponentVersions`. Adding a virtual
   to `ScriptEngine` bumps nothing.
3. **No compile-time build-ID for freshness detection.** SONAME only catches
   declared incompatibilities. The stale-binary case today was undeclared
   (SOVERSION unchanged) — a build-id hash would catch it mechanically.
4. **No startup assertion in role binaries.** Nothing calls the facility at
   main() entry.

## 4. Design

### 4.1 Extend `ComponentVersions` with a `script_engine` axis

```cpp
// plh_version_registry.hpp
struct ComponentVersions
{
    uint16_t library_major, library_minor, library_rolling;
    uint8_t  shm_major, shm_minor;
    uint8_t  wire_major, wire_minor;
    uint8_t  script_api_major, script_api_minor;
    uint8_t  script_engine_major, script_engine_minor;   // NEW
};
```

**Bump rule for `script_engine`:**
- Additive change (new virtual method with a base-class default, as added
  today): minor bump.
- Breaking change (removed/renamed virtual, changed signature, added pure
  virtual with no default): major bump.

Initial values: `script_engine_major = 1`, `script_engine_minor = 0`. The
virtual added 2026-04-21 (`pending_script_engine_request_count`) becomes the
first minor bump to `1.1` when this design lands.

### 4.2 Move per-axis constants from .cpp to header

Today `kShmMajor`, `kWireMajor`, `kScriptApiMajor`, etc. live in
`version_registry.cpp` as file-scope `constexpr`. They must move to
`plh_version_registry.hpp` as `inline constexpr` so every compile unit that
includes the header sees them.

```cpp
// plh_version_registry.hpp
inline constexpr uint8_t kShmMajorExpected           = 1;
inline constexpr uint8_t kShmMinorExpected           = 0;
inline constexpr uint8_t kWireMajorExpected          = 1;
inline constexpr uint8_t kWireMinorExpected          = 0;
inline constexpr uint8_t kScriptApiMajorExpected     = 1;
inline constexpr uint8_t kScriptApiMinorExpected     = 0;
inline constexpr uint8_t kScriptEngineMajorExpected  = 1;
inline constexpr uint8_t kScriptEngineMinorExpected  = 0;
```

### 4.3 `consteval` compile-time capture

```cpp
// plh_version_registry.hpp
// Captures the caller's view of the inline constexpr values above.
// Forces compile-time evaluation via `consteval` so the result is
// folded into the caller's binary as a constant.
consteval ComponentVersions compiled_against_here() noexcept
{
    return ComponentVersions{
        PYLABHUB_VERSION_MAJOR, PYLABHUB_VERSION_MINOR, PYLABHUB_VERSION_ROLLING,
        kShmMajorExpected,        kShmMinorExpected,
        kWireMajorExpected,       kWireMinorExpected,
        kScriptApiMajorExpected,  kScriptApiMinorExpected,
        kScriptEngineMajorExpected, kScriptEngineMinorExpected,
    };
}
```

Each translation unit that calls `compiled_against_here()` at compile time
gets the struct folded in at the value seen by THAT TU's include-tree.
When a caller recompiles against a new header (say after a `script_engine_minor`
bump to `1.1`), its next build records `1.1`. A binary compiled against the
old `1.0` header continues to carry `1.0`.

### 4.4 `AbiExpectation` bundle + build-id

```cpp
// plh_version_registry.hpp
struct AbiExpectation
{
    ComponentVersions versions;
    const char       *build_id;   // nullptr → skip build-id check
};

consteval AbiExpectation abi_expected_here() noexcept
{
    return AbiExpectation{
        compiled_against_here(),
#ifdef PYLABHUB_BUILD_ID
        PYLABHUB_BUILD_ID
#else
        nullptr
#endif
    };
}
```

`PYLABHUB_BUILD_ID` is set by CMake configure:

```cmake
# cmake/pylabhub_build_id.cmake
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    OUTPUT_VARIABLE PYLABHUB_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(PYLABHUB_BUILD_ID_VALUE "${PYLABHUB_GIT_SHA}-${CMAKE_BUILD_TYPE}")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/pylabhub_build_id.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pylabhub_build_id.h"
    @ONLY)
```

```cpp
// cmake/pylabhub_build_id.h.in
#pragma once
#define PYLABHUB_BUILD_ID "@PYLABHUB_BUILD_ID_VALUE@"
```

Both the library and every consumer include this generated header. Consumers
capture the value via `consteval`; the library reports its current value
via an exported function.

### 4.5 Runtime check function

```cpp
// plh_version_registry.hpp
struct AbiCheckResult
{
    bool         compatible;      // false → caller should abort
    std::string  message;         // human-readable one-liner
    struct {
        bool library, shm, wire, script_api, script_engine, build_id;
    } major_mismatch;
};

PYLABHUB_UTILS_EXPORT
AbiCheckResult check_abi(const ComponentVersions &expected,
                         const char *expected_build_id = nullptr) noexcept;
```

Semantics:
- For each axis: **major mismatch** → `compatible = false`.
- Any axis minor mismatch → log WARN, `compatible` unaffected.
- `expected_build_id == nullptr` → skip build-id comparison (release mode).
- `expected_build_id` non-null and differs from library's current build-id →
  `compatible = false` (strict freshness check for development).

Implementation sketch (in `version_registry.cpp`):

```cpp
AbiCheckResult check_abi(const ComponentVersions &exp,
                         const char *exp_build_id) noexcept
{
    AbiCheckResult r{true, {}, {}};
    const auto cur = current();

    auto check_major = [&r](const char *name, auto e, auto c, bool &flag) {
        if (e != c) { flag = true; r.compatible = false;
                       r.message += fmt::format("{} major {} != {}; ",
                                                name, e, c); }
    };
    check_major("library",       exp.library_major,        cur.library_major,        r.major_mismatch.library);
    check_major("shm",           exp.shm_major,            cur.shm_major,            r.major_mismatch.shm);
    check_major("wire",          exp.wire_major,           cur.wire_major,           r.major_mismatch.wire);
    check_major("script_api",    exp.script_api_major,     cur.script_api_major,     r.major_mismatch.script_api);
    check_major("script_engine", exp.script_engine_major,  cur.script_engine_major,  r.major_mismatch.script_engine);

    if (exp_build_id) {
        const char *cur_bid = PYLABHUB_BUILD_ID;
        if (std::strcmp(exp_build_id, cur_bid) != 0) {
            r.major_mismatch.build_id = true;
            r.compatible = false;
            r.message += fmt::format("build_id {} != {}; ", exp_build_id, cur_bid);
        }
    }

    // Minor-only deltas → WARN (compatible stays true).
    auto warn_minor = [&](const char *name, auto e, auto c) {
        if (e != c) LOGGER_WARN("ABI check: {} minor {} != {} (additive)", name, e, c);
    };
    warn_minor("shm",           exp.shm_minor,           cur.shm_minor);
    warn_minor("wire",          exp.wire_minor,          cur.wire_minor);
    warn_minor("script_api",    exp.script_api_minor,    cur.script_api_minor);
    warn_minor("script_engine", exp.script_engine_minor, cur.script_engine_minor);

    if (r.compatible && r.message.empty())
        r.message = "ABI OK";
    return r;
}
```

### 4.6 Caller use

```cpp
// producer_main.cpp (and every consumer binary)
int main(int argc, char *argv[])
{
    constexpr auto kExpected = pylabhub::version::abi_expected_here();
    const auto r = pylabhub::version::check_abi(
        kExpected.versions, kExpected.build_id);
    if (!r.compatible) {
        std::fprintf(stderr,
            "ABI mismatch; refusing to run: %s\n"
            "Rebuild this binary against the installed library.\n",
            r.message.c_str());
        return 2;
    }
    // ... rest of main
}
```

## 5. Modules That Should Integrate

### 5.1 Required (call `check_abi()` on startup)

| Binary / entry | File | Notes |
|---|---|---|
| `pylabhub-producer` | `src/producer/producer_main.cpp` | First statement in `main()`; deleted after Phase 20 |
| `pylabhub-consumer` | `src/consumer/consumer_main.cpp` | First statement in `main()`; deleted after Phase 20 |
| `pylabhub-processor` | `src/processor/processor_main.cpp` | First statement in `main()`; deleted after Phase 20 |
| `pylabhub-hubshell` | `src/hubshell/hubshell.cpp` | First statement in `main()`; persists post-Phase 20 |
| `plh_role` (unified) | `src/plh_role/main.cpp` (future) | Primary integration target after Phase 19 |

### 5.2 Required (already has ABI-check pattern; extend for new axis)

| Component | File | Notes |
|---|---|---|
| `NativeEngine` | `src/utils/service/native_engine.cpp` | Already calls `native_abi_info()` on loaded plugin. Extend to also pass the host's `ComponentVersions` to the plugin so the plugin can verify it compiled against compatible headers |
| Native plugin template | `src/include/utils/native_engine_api.h` | `PlhAbiInfo` struct to include `ComponentVersions` fields the plugin was compiled against; host compares on load |

### 5.3 Optional (diagnostic / test surface)

| Component | Notes |
|---|---|
| `pylabhub_abi_info_json()` | Add a field per axis + build_id (already partially covered) |
| L4 pattern-3 test framework (`shared_test_helpers.h`) | Optional: workers could log their captured AbiExpectation at startup — aids post-mortem triage when a stale test binary fails |
| Python bindings (`api.version_info_json()`) | Extend to include `script_engine` axis + build_id so scripts can introspect |
| Broker protocol (REG_REQ payload) | Consider embedding the role's ComponentVersions so the broker can reject role registrations from incompatible versions at the control-plane level |

### 5.4 Explicitly NOT in scope

- Individual utility classes (DataBlock, BrokerService, etc.) — they are library
  symbols, their version flows through `pylabhub-utils`'s SOVERSION + the
  `library` axis of the registry.
- Test executables — tests link pylabhub-utils the same way binaries do; if
  a test sees stale library it'd fail at the dynamic-link step or produce
  incorrect assertions. Catching it at test main() is nice-to-have but not
  required by the design; the workers themselves should exit nonzero on
  failure regardless.

## 6. Strict vs Tolerant Modes

| Mode | `expected_build_id` value | Behavior |
|---|---|---|
| Debug / dev builds | Non-null (from `PYLABHUB_BUILD_ID`) | Strict — any build-id delta fails |
| Release / install builds | `nullptr` (compile-time-stripped) | Tolerant — rely on major version axes only |

Controlled by a compile-time flag:

```cpp
// plh_version_registry.hpp
consteval AbiExpectation abi_expected_here() noexcept
{
#if defined(PYLABHUB_STRICT_ABI_CHECK) || !defined(NDEBUG)
    return {compiled_against_here(), PYLABHUB_BUILD_ID};
#else
    return {compiled_against_here(), nullptr};
#endif
}
```

Default: strict in Debug, tolerant in Release. Overridable via CMake
`PYLABHUB_STRICT_ABI_CHECK=ON` for CI or staging builds that want Release
optimization with Debug's stale-binary guard.

## 7. Relationship to HEP-CORE-0024 Phase 19-20

Per the binary unification plan, the three per-role binaries
(`pylabhub-producer`, `pylabhub-consumer`, `pylabhub-processor`) will be
replaced by a single `plh_role` binary driven by `--role` CLI + `RoleRegistry`.

**Implication for this design:** implementing `check_abi()` in each of the three
per-role mains is three places that will disappear. Better to defer the
consumer-side integration to the unified `plh_role` entry point and land the
library-side facility (registry extension, `check_abi()`, `consteval`
helpers, build-id plumbing) independently.

**Suggested order:**
1. **Library side** (can land any time): move constants to header, add
   `script_engine` axis, add `compiled_against_here()`, `abi_expected_here()`,
   `check_abi()`, CMake build-id.
2. **plh_role integration** (after Phase 19 lands): wire `check_abi()` at
   main() entry.
3. **Native engine extension** (any time after library side): extend
   `PlhAbiInfo` to carry ComponentVersions; extend `NativeEngine::load_plugin_()`
   to compare.
4. **hubshell** (any time): add check to `hubshell.cpp::main()`.

## 8. Estimated Cost

| Area | LOC | Notes |
|---|---|---|
| Library side (registry + check_abi + consteval) | ~80 new + ~20 moved | Mechanical |
| CMake build-id plumbing | ~15 | Configure-time only |
| Per-binary main() integration | ~10 per binary | × 1 after Phase 20 |
| NativeEngine extension | ~40 | Additive PlhAbiInfo fields |
| Tests | ~100 | Unit test for check_abi matrix of mismatch cases |

Total: ~275 LOC + ~100 test LOC.

## 9. Test Plan

Unit tests in `tests/test_layer1_base/test_abi_check.cpp` (or similar):

1. **Identity check** — pass `current()` as expected; `compatible == true`.
2. **Major library mismatch** — expected `library_major = 99`; `compatible == false`,
   `major_mismatch.library == true`, message mentions "library".
3. **Minor mismatch only** — expected `library_minor = 99`; `compatible == true`,
   WARN logged (verify via test log capture).
4. **Build-id mismatch** — pass non-matching build-id string; `compatible == false`,
   `major_mismatch.build_id == true`.
5. **Build-id nullptr** — pass `nullptr`; build-id axis skipped regardless of value.
6. **Multiple mismatches** — expected differs on multiple axes; all flags set,
   `compatible == false`, message mentions all.
7. **consteval compile-time test** — `static_assert` that
   `compiled_against_here().library_major == PYLABHUB_VERSION_MAJOR` (verifies
   the consteval machinery compiles and folds).

## 10. Open Questions

- Should `script_engine_major/minor` be per-interface (one pair per abstract
  class) or global across the scripting subsystem? **Proposed:** global;
  `ScriptEngine` is the only abstract class scripts interact with.
- Should build-id be SHA1 (git) or SHA-256 of the ELF text section?
  **Proposed:** git SHA — matches developer mental model, cheap to compute.
- Should `check_abi` be called automatically via `__attribute__((constructor))`
  or explicitly in `main()`? **Proposed:** explicit — constructors run before
  logger init and cannot report diagnostics cleanly.
- Should mismatches increment a metric or emit a structured event?
  **Proposed:** no — the process aborts before any metric pipeline is live.
  stderr + return code is sufficient.

## 11. References

- HEP-CORE-0026 — Version registry
- HEP-CORE-0032 — ABI compatibility roadmap
- HEP-CORE-0024 Phase 19-20 — `plh_role` unification
- `src/include/plh_version_registry.hpp`
- `src/utils/core/version_registry.cpp`
- `src/include/utils/native_engine_api.h` — existing `PlhAbiInfo` pattern
- Session log 2026-04-21 — root-cause analysis of the `Validate_ExitZero` SIGSEGV
