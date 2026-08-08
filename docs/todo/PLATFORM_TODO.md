# Platform TODO

**Scope:** Windows / MSVC / cross-platform / CMake / packaging.
**Strategic status:** `docs/TODO_MASTER.md`.  This file holds platform-
specific detail for open items only.

---

## Current Focus

### Compiler warnings are an index of refactor residue (2026-07-30)

A CLEAN `stage_all` emits **48 warning lines** — incremental builds
recompile too little to show them, which is how these went unnoticed.
Capture with a full rebuild, not an incremental one:
`cmake --build build --target stage_all --clean-first -j2 > log 2>&1`.

Provenance check (`git blame -w --ignore-rev <clang-format-v21 commit
86e4ec5c>` — that reformat touched 484 files and masks true blame):
every production site traces to a refactor/migration/cleanup commit,
not to fresh code.  Treat the warning list as a free mechanical index
of where refactors left stubs behind — this is the dead-code sweep
`feedback_name_collisions_and_dead_code` asks for, already done by the
compiler.  Feeds task #87.

**Genuine residue — fix these:**

- [ ] **`src/utils/hub/hub_zmq_queue.cpp:702` — `validate_curve_factory_params`
  ignores `server_pubkey_z85` AND `bind_side`.**  Highest value of the
  set.  Origin `fd118787` "#158 C2-cleanup: delete ZmqAuthOptions
  struct + `*_with_auth` factories + **validator helper**" — the body
  that read both parameters was deleted, the wide signature survived.
  The docstring still claims it validates "the CURVE auth parameters";
  it validates one of three.  `bind_side` exists so the check can
  differ (bind needs no server key, connect REQUIRES one) and that
  branch was never written; caller at `:1029` already passes
  `/*server_pubkey_z85=*/{}`.  Same shape as the #68 dead-validator
  family.  Decide: implement the missing checks, or delete the
  parameters — do not leave a validator that names what it ignores.
- [ ] `src/consumer/consumer_role_host.cpp:114` — unused `tr`.  Origin
  `206fcf56` (RoleConfig migration).  Benign: transport IS read, at
  `:321` via `config_.in_transport()` directly.  Delete the binding.
- [ ] `src/utils/service/role_config_translation.cpp:74` — unused `shm`.
  Origin `59c75f87` (M9 step 1 free-function extraction).
- [ ] `tests/test_layer2_service/workers/jsonconfig_workers.cpp:126` —
  `json_mods()` defined but unused.  Origin `65e8327c` (Pattern 3
  conversion) — same migration family as #52/#54/#56.
- [ ] `tests/test_layer4_plh_hub/test_plh_hub_role_zmq_e2e.cpp:1398` —
  `dump_all` set but not used.  Origin `f9e11c18`; a debugging aid.
- [ ] `tests/test_layer2_service/test_hub_zmq_queue.cpp:3005` — unused `r`.
- [ ] `src/utils/service/vault_crypto.cpp:40` —
  `set_owner_only_permissions` unused on POSIX.  NOT a permissions
  hole: it is called at `:105` inside the Windows branch, and the
  POSIX path uses `open(O_CREAT|O_EXCL|O_NOFOLLOW, S_IRUSR|S_IWUSR)`
  plus a belt-and-braces `fchmod(0600)`.  Guard the definition with
  the same `#if` so it stops warning.
- [ ] `src/utils/security/shm_capability_channel.cpp:972` — multi-line
  comment (`-Wcomment`), a stray trailing backslash.
- [ ] `src/scripting/lua_engine.cpp:164`, `src/scripting/python_engine.cpp:140`
  — unused parameter `core`.  Shared signature across both engines, so
  unname or `[[maybe_unused]]` on BOTH (multi-engine parity), never one
  side only.
- [ ] **`tests/test_layer3_pattern4/test_pattern4_zmq_endpoint_registry.cpp:374`
  — `-Wdangling-else`.**  Worth reading properly: an ambiguous `else`
  binding is a logic hazard, not a style nit.

**Verified benign — do NOT "fix" these:**

- `src/scripting/json_py_helpers.hpp:97`,`:104` — `-Wredundant-move`.
  Genuinely redundant, but only because the target is C++20
  (`src/scripting/CMakeLists.txt:42`): P1825R0 dropped the same-type
  requirement, so `return d;` implicit-moves a `py::dict` local into a
  `py::object` return.  Under C++17 the `std::move` was LOAD-BEARING
  (derived→base got no implicit move) — so if the standard is ever
  lowered, restore them.  No memory implication either way: `std::move`
  is a cast, and the worst case in any direction is one wasted refcount
  round-trip.  Reported 10x via 5 include paths.
- 13 x `-Wunused-result` on `[[nodiscard]]` in
  `test_known_roles.cpp`, `test_curve_keypair.cpp`,
  `test_wire_adapter_roundtrip.cpp`, `test_wire_envelope.cpp`,
  `key_store_workers.cpp` — ALL inside `EXPECT_THROW(...)`, where the
  value is discarded because the call throws.  The tests are correct.
- `tests/test_framework/broker_test_harness.cpp:303` — missing
  initializer for `channel_topology`.  It is a `std::string`, optional
  on the wire; empty == "topology not declared", a legitimate REG shape.
- `third_party/libzmq/src/session_base.cpp:458` — not ours (NO-GO list).
- 3 x `make: jobserver unavailable` — build-system noise.

### Platform support claims vs CI validation (2026-03-15)

- [ ] **CI is Linux-only** but `pyproject.toml` classifiers advertise
  macOS and Windows, and `docs/README/README_testing.md` says all tests
  must run on Windows/Linux/macOS/FreeBSD.
  `docs/README/README_utils.md` still says DataBlock-on-Windows is
  incomplete.  Either add macOS/Windows CI jobs or narrow the
  documented platform support to match what is actually validated.

### N6 (#86) — User-oriented `cmake/pylabhubNativePlugin.cmake` helper

**Drop-in CMake module for native-plugin authors** who use pylabhub
as a dependency.  Must NOT be project-internal CMake — this is a
deliverable for downstream plugin authors.

Plugin author drops the module into their own project, calls:

```cmake
find_package(pylabhubNativePlugin REQUIRED
             PATHS /path/to/pylabhub/install)
plh_add_native_plugin(my_plugin SOURCES my_plugin.cpp)
```

Helper deduces include + lib paths from the install location;
provides the `plh_add_native_plugin(target SOURCES files...)` macro
that handles `-shared -fPIC -std=c++20 -I<include>` plus correct
link visibility (header-only `native_engine_api.h` — no link to
`pylabhub::utils` for plugins using the C API; optional link for
the `plh::Context` C++ wrapper).

Pairs with `docs/README/README_NativePlugins.md` (task #86).

### CMake test-staging: no wrapper binds register + discover (deferred 2026-07-25)

Every test target must call **both** `pylabhub_register_test_for_staging(TARGET x)`
and `gtest_discover_tests(x ...)` by hand (~40 call-site pairs).  Forget the
first and the binary silently never stages into the ctest run — a real
drift/fragility risk with no compile-time guard.  Add a single
`pylabhub_add_gtest(...)` wrapper in `cmake/StageHelpers.cmake` that performs
register + discover together, then migrate the call sites.  Deferred as its
own pass (broad-but-cosmetic churn); the three functional cmake fixes shipped
in commit `21b8696f`.

---

## Backlog

### Clang-tidy quality pass

> **This section holds the PROCEDURE; `docs/code_review/LINT_FIXES_PLAN.md`
> holds the RESULTS — and those results are stale (April 2026 log, partially
> pointing at deleted files).  The two halves lived apart with no
> cross-reference until 2026-08-07.  Run the recipe below to regenerate, then
> diff against that plan before actioning any of it.  Tracked as a task.**

- [ ] Reconfigure with `CC=clang CXX=clang++` and
  `-DPYLABHUB_ENABLE_CLANG_TIDY=ON` for a complete static-analysis
  sweep.  GCC build is clean; clang-tidy adds cppcoreguidelines,
  modernize, etc.  Infra wired in `cmake/PlatformAndCompiler.cmake`.
  Run periodically.

```bash
CC=clang CXX=clang++ cmake -S . -B build-clang \
    -DPYLABHUB_ENABLE_CLANG_TIDY=ON
cmake --build build-clang 2>&1 | grep -E "warning:|error:" | \
    grep -v third_party
```

### Windows (MSVC) — known gaps

- [ ] **`/Zc:preprocessor` PUBLIC propagation audit.**  Confirm all
  consumers of `pylabhub::utils` that use `PLH_DEBUG` / `LOGGER_*`
  macros receive the flag via CMake interface propagation.  Run a
  Windows CI build targeting at least one consumer executable and
  verify no C3878 / C2760 VA_OPT errors.  Site:
  `src/utils/CMakeLists.txt`.

- [ ] **MSVC warnings-as-errors gate.**  Add `/W4 /WX` to MSVC CI to
  catch future C4251 / C4324 / C4996 regressions early.  Currently
  only runs on Linux with `-Werror`.

---

## Notes

### MSVC vs GCC differences to remember
- MSVC requires `/Zc:preprocessor` for `__VA_OPT__` / variadic-macro
  semantics consistent with GCC + Clang.
- DLL boundary: types crossing PUBLIC API need explicit
  `PYLABHUB_UTILS_EXPORT` annotation (see HEP-CORE-0032).
- `_CRT_SECURE_NO_WARNINGS` not blanket-set — site-by-site `#pragma
  warning(suppress)` instead.

### Pattern: FlexZone / DataBlock POD requirement
DataBlock + FlexZone payloads must be POD (standard-layout +
trivially-copyable) for cross-platform memory layout to match.
Test: `std::is_standard_layout<T>::value && std::is_trivially_
copyable<T>::value`.  Enforced at the schema-validation layer.

---

## Related Work

- `docs/HEP/HEP-CORE-0032-ABI-Compatibility.md` — ABI policy for
  cross-toolchain builds.
- `docs/README/README_CMake_Design.md` — project build structure.

## `check_auth_guardrail.sh` has no Windows mirror and is not platform-gated

Found 2026-08-07 while adding `SecurityGuardrail_SodiumConfinedToModule`.

`AuthGuardrail_NoScriptAllowlistMutator` is registered in
`tests/CMakeLists.txt` with a bare `add_test(... COMMAND
${CMAKE_SOURCE_DIR}/tools/check_auth_guardrail.sh ...)` — **no `if(WIN32)`
branch and no `.ps1` mirror.** The two guardrails added after it
(`check_layer_invariant`, `check_fixtures_required`) both ship `.sh` +
`.ps1` behind a `WIN32` conditional, so the convention post-dates it.

On Windows that test cannot run. Worse than a gap in coverage: it carries
`FIXTURES_SETUP Guardrails`, and every gtest target requires that fixture,
so a failing setup fixture takes the **whole suite** with it.

Fix: write `tools/check_auth_guardrail.ps1` mirroring the `.sh` (one regex
over a fixed path list — the smallest of the four) and wrap the `add_test`
in the same `if(WIN32)/else()` shape the others use.

Not folded into the sodium-guardrail commit that found it: that change was
scoped to adding a new check, and silently repairing a different test's
platform wiring inside it would hide the fix from anyone reading the log.
