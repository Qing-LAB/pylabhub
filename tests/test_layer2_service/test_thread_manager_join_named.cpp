/**
 * @file test_thread_manager_join_named.cpp
 * @brief Pattern 3 driver — `ThreadManager::join_named` tests.
 *
 * `join_named` is the per-slot signal+wait+join primitive (HEP-CORE-0033
 * §4.2 step 2 — HubHost shutdown ordering).  The body of each test
 * constructs a `ThreadManager`, which registers a dynamic lifecycle
 * module on `Logger`; per `docs/README/README_testing.md` § "Choosing
 * a test pattern" the body must run in a worker subprocess.  Worker
 * bodies live in `workers/thread_manager_join_named_workers.cpp` and
 * register their dispatcher at static-init time.
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern explicitly ruled out by
 * `docs/README/README_testing.md` § "Antipatterns".
 *
 * Contract under test (each scenario pins both an outcome AND a
 * path-discriminating side effect — see CLAUDE.md "Testing Practice"
 * rule + IG § "Testing Strategy"):
 *
 *   - Happy path: signal → bounded join returns true; slot removed.
 *   - Unknown name: returns false without side effects.
 *   - Idempotent: second call returns false (slot already removed).
 *   - Bounded-join expiry: thread that ignores the stop signal is
 *     detached after `join_timeout`; returns false.
 *   - Bracketed thread observes internal `shutdown_requested` signal
 *     set by `join_named` before the bounded wait (MD1.5 contract).
 *   - Cooperates with later `drain()`: unjoined siblings still drain.
 *   - After `drain()`: `join_named` refuses (closing flag set).
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class ThreadManagerJoinNamedTest : public IsolatedProcessTest
{
};

TEST_F(ThreadManagerJoinNamedTest, HappyPath_SignalThenJoin_ReturnsTrue)
{
    auto w = SpawnWorker(
        "thread_manager_join_named.happy_path_signal_then_join");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerJoinNamedTest, UnknownName_ReturnsFalse_NoSideEffects)
{
    auto w = SpawnWorker(
        "thread_manager_join_named.unknown_name_returns_false");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerJoinNamedTest, Idempotent_SecondCallReturnsFalse)
{
    auto w = SpawnWorker(
        "thread_manager_join_named.idempotent_second_call");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerJoinNamedTest, UncooperativeThread_DetachedAfterTimeout)
{
    auto w = SpawnWorker(
        "thread_manager_join_named.uncooperative_thread_detached");
    // This test deliberately exercises the detach-on-timeout path,
    // which emits a stage-differentiated ERROR log.  Declare the
    // expected substring up-front so the worker framework accepts it.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_errors=*/{"detaching"});
}

TEST_F(ThreadManagerJoinNamedTest, BracketedThread_ObservesInternalSignalAndExits)
{
    auto w = SpawnWorker(
        "thread_manager_join_named.bracketed_thread_observes_internal_signal");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerJoinNamedTest, CooperatesWithDrain_UnjoinedSiblingsDrain)
{
    auto w = SpawnWorker(
        "thread_manager_join_named.cooperates_with_drain");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerJoinNamedTest, AfterDrain_RefusesNewJoin)
{
    auto w = SpawnWorker(
        "thread_manager_join_named.after_drain_refuses_new_join");
    ExpectWorkerOk(w);
}
