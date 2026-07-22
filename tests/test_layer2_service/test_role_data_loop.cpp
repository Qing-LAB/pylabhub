/**
 * @file test_role_data_loop.cpp
 * @brief Pattern 3 driver: run_data_loop + ThreadManager unit tests.
 *
 * Each TEST_F spawns a worker subprocess.  Under HEP-CORE-0036 §8.2
 * the data loop is protocol-gated by `any_presence_authorized()`;
 * the L2 tests honor this by legitimately constructing a `Presence`
 * with `registration_state = Authorized` and installing it on the
 * `RoleAPIBase` via `RoleAPIBaseTestAccess::install_handler` — the
 * gate runs unchanged afterwards.  See
 * `tests/test_framework/role_api_base_test_access.h` for the
 * no-bypass contract.
 */
#include "test_patterns.h"

#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class RunDataLoopTest : public IsolatedProcessTest
{
};

// L2 protocol-gate test access is physically gated to
// `PYLABHUB_BUILD_TESTS && !defined(NDEBUG)` (mirrors
// `role_host_core.hpp:581` `test_set_*` mutators).  Outside
// that build configuration the friend declaration + private setter
// on `RoleAPIBase` don't exist, the helper symbol is absent, the
// worker scenarios can't install a Presence to satisfy the §8.2
// outer guard, so the tests skip cleanly instead of failing with
// link or runtime errors.
//
// 2026-07-04: converted from a static helper to a macro so
// `GTEST_SKIP()` returns from the TEST_F, not from the helper.
// Prior helper-based form left the TEST_F running past the SKIP
// point into `SpawnWorker(...)`, which then died with "unknown
// scenario" in Release builds (the scenarios are compiled out
// alongside the test-mutators).  Only visible after the 2026-07-04
// sodium_init fix let Release CI reach these tests.
#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
#define SKIP_IF_NO_TEST_ACCESS() ((void)0)
#else
#define SKIP_IF_NO_TEST_ACCESS()                                                                   \
    GTEST_SKIP() << "Built without PYLABHUB_BUILD_TESTS+Debug — "                                  \
                 << "RoleAPIBase protocol-gate test access is compiled out (mirrors "              \
                 << "role_host_core.hpp:581 test-mutator gating)."
#endif

TEST_F(RunDataLoopTest, ShutdownStopsLoop)
{
    SKIP_IF_NO_TEST_ACCESS();
    auto w = SpawnWorker("role_data_loop.shutdown_stops_loop", {});
    ExpectWorkerOk(w);
}

TEST_F(RunDataLoopTest, InvokeReturnsFalseStopsLoop)
{
    SKIP_IF_NO_TEST_ACCESS();
    auto w = SpawnWorker("role_data_loop.invoke_returns_false_stops_loop", {});
    ExpectWorkerOk(w);
}

// HEP-CORE-0011 §"Loop-ready gate" — AND composition invariant pin.
// Framework default returns NotReady + user script `on_init` returns
// Ready: the AND-composed gate MUST hold and the loop must NEVER call
// acquire.  Load-bearing regression guard against:
//   - composition changing from AND to OR
//   - short-circuit that skips the framework default when a script
//     hook is present
//   - init_timeout_ms budget silently disabled
TEST_F(RunDataLoopTest, FrameworkFloorHoldsGate)
{
    SKIP_IF_NO_TEST_ACCESS();
    auto w = SpawnWorker("role_data_loop.framework_floor_holds_gate", {});
    // The scenario deliberately drives the framework floor NotReady so
    // the loop-ready gate holds until `init_timeout_ms` elapses; that
    // path emits an ERROR-level `event=LoopInitTimeout` log line by
    // design.  Declare it expected so the ERROR-scanning inside
    // `ExpectWorkerOk` doesn't treat the intended failure signal as
    // an unexpected error.
    ExpectWorkerOk(w, /*required_substrings=*/{},
                   /*expected_error_substrings=*/{"event=LoopInitTimeout"});
}

TEST_F(RunDataLoopTest, MetricsIncrement)
{
    SKIP_IF_NO_TEST_ACCESS();
    auto w = SpawnWorker("role_data_loop.metrics_increment", {});
    ExpectWorkerOk(w);
}

TEST_F(RunDataLoopTest, NoDataSkipsDeadlineWait)
{
    SKIP_IF_NO_TEST_ACCESS();
    auto w = SpawnWorker("role_data_loop.no_data_skips_deadline_wait", {});
    ExpectWorkerOk(w);
}

TEST_F(RunDataLoopTest, OverrunDetected)
{
    SKIP_IF_NO_TEST_ACCESS();
    auto w = SpawnWorker("role_data_loop.overrun_detected", {});
    ExpectWorkerOk(w);
}

class ThreadManagerTest : public IsolatedProcessTest
{
};

TEST_F(ThreadManagerTest, SpawnAndJoin)
{
    auto w = SpawnWorker("role_data_loop.thread_manager_spawn_and_join", {});
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerTest, MultipleThreads)
{
    auto w = SpawnWorker("role_data_loop.thread_manager_multiple_threads", {});
    ExpectWorkerOk(w);
}

// Test was previously named `JoinInReverseOrder` but the worker only
// asserts `order.size() == 2` — i.e. that drain() waits for ALL
// spawned threads to complete (including a slow one).  It does NOT
// observe the order in which drain() invokes `join()` on the slots.
// Verifying actual reverse-spawn-order join from outside the
// ThreadManager would require exposing internal join-sequence state
// (e.g. a per-slot "joined_at" timestamp), which is not currently
// part of the public surface.  Renamed to match what's actually
// tested; reverse-order verification is a separate follow-up.
TEST_F(ThreadManagerTest, DrainJoinsAllThreads)
{
    auto w = SpawnWorker("role_data_loop.thread_manager_join_in_reverse_order", {});
    ExpectWorkerOk(w);
}
