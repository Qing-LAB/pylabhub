# Code Review Guidance

**Purpose:** Instructions for thorough and critical code review: what to check first, higher-level requirements, and test integration after review. This document is a **draft framework**; improve it with additional discussion and project-specific learnings.

**Doc policy:** This guidance complements **`docs/IMPLEMENTATION_GUIDANCE.md`** (implementation patterns and checklist) and **`docs/DOC_STRUCTURE.md`** (documentation layout). Execution plan and priorities live in **`docs/DATAHUB_TODO.md`**. For test rationale and phase detail, see **`docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md`**.

---

## Table of Contents

1. [Project Context: Structure and Where to Look](#1-project-context-structure-and-where-to-look)
2. [First Pass: Basic Checks](#2-first-pass-basic-checks)
3. [Higher-Level Requirements](#3-higher-level-requirements)
4. [Test Integration After Review](#4-test-integration-after-review)
5. [Review Workflow Summary](#5-review-workflow-summary)

Sections **2.6** and **2.7** integrate code-quality and warning practices (duplication, refactoring, Doxygen, `[[nodiscard]]`, exception specs) as standard review items.

---

## 1. Project Context: Structure and Where to Look

Reviewers should know where code and scripts live, how the build is organized, and which docs to cross-check. Use this section to orient before reviewing.

### 1.1 Documentation Layout

| Need | Document |
|------|----------|
| What to do next / roadmap | **`docs/DATAHUB_TODO.md`** |
| How to implement / patterns / checklist | **`docs/IMPLEMENTATION_GUIDANCE.md`** |
| Doc structure and naming | **`docs/DOC_STRUCTURE.md`** |
| DataHub design spec | **`docs/hep/HEP-CORE-0002-DataHub-FINAL.md`** (and other HEPs in `docs/hep/`) |
| Test plan and Phase A–D | **`docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md`** |
| Topic summaries | **`docs/README/`** (e.g. README_DataHub.md, README_testing.md, README_CMake_Design.md) |

### 1.2 Build System and Targets

- **Configure (from `cpp/`):**
  - `cmake -S . -B build` (default Debug)
  - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
  - `cmake -S . -B build -DPYLABHUB_USE_SANITIZER=Address` (ASan)
- **Build:** `cmake --build build`; `cmake --build build --target stage_all` stages all artifacts.
- **Outputs:** `build/stage-<buildtype>/` with `bin/`, `lib/`, `tests/`, `include/` (see **`docs/README/README_CMake_Design.md`**).

**Key targets:**

| Target | Purpose |
|--------|---------|
| `pylabhub-basic` | Static lib; Layer 0/1 base (spinlock, guards, platform). Cannot depend on `pylabhub-utils`. |
| `pylabhub-utils` | Shared lib; Layer 2/3 (Logger, FileLock, Lifecycle, MessageHub, DataBlock). Link via **`pylabhub::utils`**. |
| `test_layer0_platform` | Platform, version API, `shm_*` |
| `test_layer1_*` | Spinlock, recursion_guard, scope_guard, formattable |
| `test_layer2_*` | Lifecycle, FileLock, Logger, JsonConfig, SharedSpinLock, etc. |
| `test_layer3_datahub` | DataHub: schema, recovery, slot protocol, phase A, error handling, MessageHub |
| `stage_tests` / `stage_all` | Populate staging directory for running tests and binaries |

### 1.3 Source and Script Locations

| Area | Location | Notes |
|------|----------|--------|
| **Public headers** | `cpp/src/include/` | Layered umbrella headers: `plh_platform.hpp`, `plh_base.hpp`, `plh_service.hpp`, `plh_datahub.hpp`. Utils under `utils/`. |
| **Library source** | `cpp/src/utils/` (and other `src/` subdirs) | Implementation (`.cpp`) for `pylabhub-utils`; **do not modify `third_party/`** unless instructed. |
| **Tests** | `cpp/tests/` | `test_framework/` (shared infra), `test_layer0_platform/`, `test_layer1_base/`, `test_layer2_service/`, `test_layer3_datahub/`. |
| **Examples** | `cpp/examples/` | e.g. `datahub_producer_example.cpp`. |
| **Scripts / tools** | `cpp/tools/` | e.g. `format.sh` for code formatting. |
| **CMake** | Root and per-directory `CMakeLists.txt`, `cmake/` helpers | Cross-reference before proposing build changes. |

### 1.4 Conventions (CLAUDE.md)

- **Style:** `.clang-format` (LLVM-based, 4-space indent, 100-char lines, Allman braces). `.clang-tidy` with Clang treats warnings as errors.
- **Layered includes:** Prefer one umbrella header per layer; they pull in transitive includes.
- **ABI:** Public classes in `pylabhub-utils` use **pImpl**; destructors defined in `.cpp`.

---

## 2. First Pass: Basic Checks

Do these before deep design or concurrency review. They catch common issues and ensure the change builds and fits project norms.

### 2.1 Build and Sanity

- [ ] **Configure and build** from a clean or incremental build: `cmake -S cpp -B cpp/build && cmake --build cpp/build`. No configure or compile errors.
- [ ] **Relevant targets** (e.g. library + the test executable that exercises the change) build successfully.
- [ ] **Staging** (if tests or examples are touched): `cmake --build cpp/build --target stage_tests` (or `stage_all`) completes; runnables are under `build/stage-<buildtype>/`.

### 2.2 Style and Linting

- [ ] **Format:** Code conforms to project style (run `./tools/format.sh` from `cpp/` if applicable).
- [ ] **Linter:** No new clang-tidy (or equivalent) warnings; project may treat warnings as errors.
- [ ] **No stray debug code:** No commented-out debug prints, temporary `#if 0`, or leftover TODOs that should be tracked in DATAHUB_TODO.

### 2.3 Includes and Exports

- [ ] **Includes:** Correct layer usage (no unnecessary higher-layer includes in lower-layer code). `pylabhub-basic` must not depend on `pylabhub-utils`.
- [ ] **Exports:** Public API symbols in `pylabhub-utils` use **`PYLABHUB_UTILS_EXPORT`** where required for shared library visibility.

### 2.4 Obvious Correctness

- [ ] **Resource ownership:** RAII where appropriate; no leaks (handles, file descriptors, shared memory).
- [ ] **Error paths:** Error conditions are handled (return codes in C API; exceptions or returns in C++ where documented); no silent ignore of errors.
- [ ] **Magic numbers / hardcoded paths:** Avoid; use named constants or config. Document any remaining ones.

### 2.5 Documentation

- [ ] **Comments:** Non-obvious logic and public API contracts are explained.
- [ ] **Docs in sync:** If behavior or design changed, relevant docs are updated (HEP, IMPLEMENTATION_GUIDANCE, or README as per DOC_STRUCTURE).

### 2.6 Code Quality and Maintainability (common practice)

Reviewers should routinely check the following; see **`docs/CODE_QUALITY_AND_REFACTORING_ANALYSIS.md`** for detailed patterns and priorities.

- [ ] **Duplication and redundancy:** No repeated blocks that could be a shared helper (e.g. timeout/backoff, handle construction, buffer pointer calculation). Obsolete or deprecated code is either removed or clearly marked and tracked.
- [ ] **Layered design and abstraction:** Public API follows the intended layers (e.g. DataBlock: prefer transaction API; C API only where justified). Common tasks are abstracted; no unnecessary bypass of abstraction.
- [ ] **Refactoring opportunities:** Where the same logic appears in multiple places, consider extracting a common function and document in CODE_QUALITY_AND_REFACTORING_ANALYSIS if deferred.
- [ ] **Naming and consistency:** Variable names are consistent (e.g. `slot_index` vs `slot_id`); comments match the code; no misleading or stale comments.
- [ ] **Developer-friendly comments:** High-risk or subtle areas are commented (e.g. TOCTTOU, zombie reclaim, single-point creation, ABI-sensitive layout). Things that need attention are called out.
- [ ] **Doxygen and public API:** Public classes and public API functions have Doxygen (`@brief`, `@param`, `@return` as appropriate). Lifetime and thread-safety notes where relevant.
- [ ] **Build warnings:** Build completes with **zero warnings** (e.g. exception spec mismatch, unused result for `[[nodiscard]]`, unused captures). See §2.7 for `[[nodiscard]]` and exception specs.
- [ ] **`[[nodiscard]]` usage:** Functions that return a meaningful result (e.g. success/failure) keep `[[nodiscard]]`; call sites must **check and handle** the return (log, propagate error, or fail). Do not ignore with `(void)` unless the API is explicitly documented as best-effort and we have decided not to use `[[nodiscard]]` for it. **We lead by example:** in our own code we check returns from `[[nodiscard]]` APIs and handle failure (e.g. log warning, set error code); otherwise we undermine the expectation that users should check. Remove `[[nodiscard]]` only where the return value is genuinely optional and the API documents that.

### 2.7 Warnings and Exception Specifications

- [ ] **No implicit exception spec mismatch:** Destructors and other functions declared `noexcept` in the header must be defined with `noexcept` in the implementation (e.g. `~Foo() noexcept = default;` in the .cpp).
- [ ] **No unused-result warnings:** For functions marked `[[nodiscard]]`, every call site **checks and handles** the return (log, propagate, or fail). Use `(void)expr;` only where documented in **`docs/NODISCARD_DECISIONS.md`** (e.g. specific tests that intentionally do not check). Any new “intentionally ignore” site must be added there with rationale for discussion.

---

## 3. Higher-Level Requirements

After the first pass, check design and project-specific requirements. Use IMPLEMENTATION_GUIDANCE and the relevant HEP as the source of truth.

### 3.1 Architecture and ABI

- [ ] **pImpl:** All public classes in `pylabhub-utils` use the pImpl idiom; private members only in `Impl` in `.cpp`; destructor defined in `.cpp`.
- [ ] **Layered API:** DataBlock: prefer Layer 2 (transaction API / guards); use C API or lower layers only when justified (e.g. hot path, C bindings). No inappropriate bypass of abstraction.
- [ ] **Dual library rule:** `pylabhub-basic` has no dependency on `pylabhub-utils`; link to **`pylabhub::utils`** (alias target), not the raw library name.

### 3.2 Design and Spec Alignment

- [ ] **HEP alignment:** Changes that affect behavior or contract match the relevant HEP (e.g. HEP-CORE-0002 for DataHub). HEP implementation status section (if present) is updated or referenced from DATAHUB_TODO.
- [ ] **Lifecycle:** If the code registers with the lifecycle (init/shutdown), it uses `ModuleDef` and `GetLifecycleModule()`; ordering and teardown are correct. Code that uses DataBlock/MessageHub must run after the Data Exchange Hub (and typically Logger, CryptoUtils) is initialized via `LifecycleGuard` in main(); factory functions enforce this. See **`docs/IMPLEMENTATION_GUIDANCE.md`** § Lifecycle and **`src/hubshell.cpp`**.
- [ ] **Config and single point of access:** DataBlock creation validates config **before** any memory creation; required parameters are explicit (no silent defaults for policy, consumer_sync_policy, physical_page_size, ring_buffer_capacity). See IMPLEMENTATION_GUIDANCE and DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.

### 3.3 Concurrency and Correctness

- [ ] **Memory ordering:** Atomics use correct ordering (e.g. acquire/release for cross-thread visibility); ARM and x86 considerations as in IMPLEMENTATION_GUIDANCE.
- [ ] **TOCTTOU / races:** No time-of-check-time-of-use races (e.g. slot state checked then used); reader/writer protocol and reclaim logic are consistent.
- [ ] **PID and zombie reclaim:** PID reuse and zombie writer/reader reclaim (e.g. `is_process_alive`) are handled where applicable.
- [ ] **Checksums and schema:** If checksums or schema validation are in scope, they are validated on the right paths; heartbeats updated for consumers where required.

### 3.4 Error Handling and Observability

- [ ] **C vs C++:** C API uses return codes (no exceptions); C++ may throw for contract/config violations; hot path avoids unnecessary throws.
- [ ] **noexcept:** Public API that is not supposed to throw is marked `noexcept`; nothing that can throw is marked `noexcept`.
- [ ] **Logging and metrics:** Errors logged (e.g. LOGGER_ERROR/WARN); metrics updated on error paths where applicable.

### 3.5 Performance (Where Relevant)

- [ ] **Hot path:** No unnecessary exceptions or heavy work in hot paths.
- [ ] **Atomics:** Relaxed ordering only where safe; backoff for spin loops; no unnecessary barriers.

Use the **Code Review Checklist** in **`docs/IMPLEMENTATION_GUIDANCE.md`** (Before Submitting PR, Design Review, Performance Review) as the authoritative project checklist; this section summarizes and situates it. For duplication, refactoring opportunities, Doxygen gaps, and developer-facing comments, see **`docs/CODE_QUALITY_AND_REFACTORING_ANALYSIS.md`**.

---

## 4. Test Integration After Review

Before approving, ensure tests exist and are run so that the change is validated in the project’s test environment.

### 4.1 Test Structure (Quick Reference)

- **Layer 0:** `test_layer0_platform` — platform, version, `shm_*`.
- **Layer 1:** `test_layer1_*` — base utilities (spinlock, guards).
- **Layer 2:** `test_layer2_*` — services (FileLock, Logger, Lifecycle, etc.); multi-process workers where applicable.
- **Layer 3:** `test_layer3_datahub` — DataHub protocol, schema, recovery, slot protocol, MessageHub, error handling.

Details: **`docs/README/README_testing.md`**; test plan and phases: **`docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md`**.

### 4.2 What to Run After Review

- [ ] **Build and stage tests:** `cmake --build cpp/build --target stage_tests` (or rely on `PYLABHUB_STAGE_ON_BUILD=ON`).
- [ ] **Full suite (recommended before merge):**  
  `ctest --test-dir cpp/build --output-on-failure`  
  or from `cpp/build`: `ctest --output-on-failure`.
- [ ] **Relevant subset:** Run the layer or suite that covers the change, e.g.  
  `ctest --test-dir cpp/build -R "^DataBlockTest"`  
  `ctest --test-dir cpp/build -R "test_layer3_datahub"`  
  or run the staged executable with a filter, e.g.  
  `./build/stage-debug/tests/test_layer3_datahub --gtest_filter=SlotProtocolTest.*`
- [ ] **Sanitizers (if available):** e.g. AddressSanitizer build and run:  
  `cmake -S cpp -B cpp/build-asan -DPYLABHUB_USE_SANITIZER=Address` then build and run tests. Fix any reported issues before merge.
- [ ] **Multi-process / concurrency:** If the change affects IPC or concurrency, run the corresponding multi-process or concurrent tests (see test plan and README_testing).

### 4.3 Test Quality

- [ ] **New behavior covered:** New or modified behavior has corresponding tests (unit or integration as appropriate).
- [ ] **No regressions:** Existing tests that touch the changed area still pass; no unnecessary disable or skip without a documented reason.
- [ ] **Cross-platform:** Tests are run (or documented as skipped) on all supported platforms where applicable; no silent platform-only assumptions.

---

## 5. Review Workflow Summary

1. **Orient:** Use §1 to locate changed files, build targets, and relevant docs (HEP, IMPLEMENTATION_GUIDANCE, DATAHUB_TODO, test plan).
2. **First pass (§2):** Build, format/lint, includes/exports, obvious correctness, docs, and code quality (§2.6: duplication, abstraction, naming, comments, Doxygen). Build must be warning-free (§2.7: exception specs, `[[nodiscard]]`).
3. **Deep pass (§3):** Architecture (pImpl, layers, ABI), design (HEP, lifecycle, config), concurrency (ordering, TOCTTOU, PID), error handling, performance; cross-check with IMPLEMENTATION_GUIDANCE checklist.
4. **Test integration (§4):** Run appropriate tests (full or subset), sanitizers if applicable, multi-process if relevant; confirm coverage and no regressions.
5. **Decide:** Approve, request changes, or escalate; capture any project-specific follow-ups (e.g. DATAHUB_TODO items or doc updates).

---

**Revision History**

- **Draft** (2026-02-13): Initial draft framework for code review guidance; to be refined with additional discussion.
