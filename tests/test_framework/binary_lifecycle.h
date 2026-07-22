#pragma once
/**
 * @file binary_lifecycle.h
 * @brief Binary-wide `LifecycleGuard` for in-process Pattern 1/2 tests
 *        whose subject reaches a lifecycle module (`Logger`, etc.) but
 *        has no inter-test interference.
 *
 * ─────────────────────────────────────────────────────────────────
 * Scope, vs. the other test-framework lifecycle mechanisms
 * ─────────────────────────────────────────────────────────────────
 *
 *   `run_gtest_worker(...)` / `run_worker_bare(...)`  (Pattern 3)
 *     One LifecycleGuard PER subprocess.  Parent process has none.
 *     Crash isolation; libzmq/luajit/libsodium static-dtor hang
 *     dodged by `_exit()`.  See shared_test_helpers.h.
 *
 *   `SetUpTestSuite` LifecycleGuard owned by a TEST fixture
 *     Antipattern.  Re-runs `initialize()` on multi-suite binaries;
 *     hides 60-s static-dtor hangs.  Migration target of the
 *     Pattern-3 wave.  See `docs/README/README_testing.md`
 *     § "Antipatterns (forbidden)".
 *
 *   `BinaryLifecycleEnvironment` (this header)
 *     ONE LifecycleGuard for the binary's RUN_ALL_TESTS scope,
 *     installed via gtest's native `::testing::Environment` hook
 *     (registered with `::testing::AddGlobalTestEnvironment`).
 *     Lives for the duration of `RUN_ALL_TESTS`; finalized before
 *     main() returns.  Subprocess workers (`run_gtest_worker`) are
 *     UNAFFECTED — they spawn fresh processes that don't inherit
 *     the parent's environment, and install their own guard.
 *
 * ─────────────────────────────────────────────────────────────────
 * When to use this
 * ─────────────────────────────────────────────────────────────────
 *
 *   The subject under test reaches a lifecycle module (e.g., emits
 *   `LOGGER_*`), BUT careful examination shows no cross-test
 *   interference is possible (no static state in subject; no
 *   init-once invariant being re-violated; no library-global
 *   destructor hang risk).  Pattern 3 subprocess isolation provides
 *   no real benefit in that case, while costing ~50 ms fork overhead
 *   per test.
 *
 *   `BinaryLifecycleEnvironment` initializes the lifecycle modules
 *   ONCE for the binary's gtest run, so `LogCaptureFixture::Install`
 *   per-test works (regression guard "no unexpected WARN") and tests
 *   run in-process at ~5 ms instead of ~55 ms.
 *
 *   Use this ONLY after examining:
 *     1. Static state in the subject classes (none?)
 *     2. Init-once invariants (single suite per binary, not violated?)
 *     3. Library-global static-dtor hang risk at program exit (the
 *        binary cleans up lifecycle state before main() returns —
 *        the Environment's TearDown drives this)
 *     4. Crash isolation needs (this binary's tests don't crash
 *        deliberately?)
 *
 *   If any of (1)-(4) raises a concern, use Pattern 3 instead.
 *
 * ─────────────────────────────────────────────────────────────────
 * Usage
 * ─────────────────────────────────────────────────────────────────
 *
 * In any TU of the test binary, at file scope:
 *
 *     #include "binary_lifecycle.h"
 *     #include "utils/logger.hpp"
 *
 *     PLH_BINARY_LIFECYCLE_MODULES(
 *         pylabhub::utils::Logger::GetLifecycleModule()
 *     )
 *
 * Then write your tests as plain `::testing::Test` fixtures (with or
 * without `LogCaptureFixture` mixin).  No `SetUpTestSuite`-owned
 * LifecycleGuard.  No subprocess workers.
 *
 * Coexists with Pattern 3 tests in the same binary: subprocess
 * workers spawn fresh and install their own guard via
 * `run_gtest_worker`, never seeing the parent's
 * `BinaryLifecycleEnvironment`.
 */

#include "utils/lifecycle.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <source_location>
#include <vector>

namespace pylabhub::tests
{

/// gtest `::testing::Environment` that owns a binary-wide
/// `pylabhub::utils::LifecycleGuard` for the duration of
/// `RUN_ALL_TESTS`.  Register with `::testing::AddGlobalTestEnvironment`
/// (typically via the `PLH_BINARY_LIFECYCLE_MODULES` macro below).
class BinaryLifecycleEnvironment : public ::testing::Environment
{
  public:
    template <typename... Mods>
    explicit BinaryLifecycleEnvironment(Mods &&...mods)
        : mods_(pylabhub::utils::MakeModDefList(std::forward<Mods>(mods)...))
    {
    }

    void SetUp() override
    {
        // `LifecycleGuard(std::vector<ModuleDef>&&, source_location)`
        // moves the vector into the manager.  gtest calls Environment
        // SetUp exactly once per `RUN_ALL_TESTS`, so the move-out is
        // safe; mods_ is left empty after this.
        guard_.emplace(std::move(mods_), std::source_location::current());
    }

    void TearDown() override { guard_.reset(); }

  private:
    std::vector<pylabhub::utils::ModuleDef> mods_;
    std::optional<pylabhub::utils::LifecycleGuard> guard_;
};

} // namespace pylabhub::tests

/// One-line registration of a binary-wide LifecycleGuard.  Place at
/// file scope in any TU compiled into the test binary.  Idempotent
/// w.r.t. worker-mode invocations: workers return from
/// `test_entrypoint.cpp` main() before `RUN_ALL_TESTS`, so the
/// environment's SetUp/TearDown never fire on that path.
///
/// Args: a comma-separated module list, same shape as
/// `MakeModDefList(...)` / `run_gtest_worker(..., mods...)`.
///
/// Example:
///     PLH_BINARY_LIFECYCLE_MODULES(
///         pylabhub::utils::Logger::GetLifecycleModule(),
///         pylabhub::utils::FileLock::GetLifecycleModule()
///     )
#define PLH_BINARY_LIFECYCLE_MODULES(...)                                                          \
    namespace                                                                                      \
    {                                                                                              \
    struct PlhBinaryLifecycleRegistrar                                                             \
    {                                                                                              \
        PlhBinaryLifecycleRegistrar()                                                              \
        {                                                                                          \
            ::testing::AddGlobalTestEnvironment(                                                   \
                new ::pylabhub::tests::BinaryLifecycleEnvironment{__VA_ARGS__});                   \
        }                                                                                          \
    } g_plh_binary_lifecycle_registrar;                                                            \
    }
