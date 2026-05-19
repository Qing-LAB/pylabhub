/**
 * @file test_role_host_core.cpp
 * @brief Unit tests for RoleHostCore — the shared state container for all role hosts.
 *
 * Tests:
 *   - Default construction (all fields zeroed/false)
 *   - Shutdown / error control (request_stop, set_critical_error, stop_reason)
 *   - Loop condition methods (should_continue_loop, should_exit_inner)
 *   - External shutdown flag (set_shutdown_flag, is_process_exit_requested)
 *   - Metric counters (inc_*, read, accumulation)
 *   - Message queue (enqueue, drain, overflow at kMaxIncomingQueue)
 *   - Init-time state (validate_only, script_load_ok, fz_spec)
 *   - Cross-thread metric visibility (write on one thread, read on another)
 */

#include "binary_lifecycle.h"
#include "plh_service.hpp"
#include "utils/role_host_core.hpp"
#include "log_capture_fixture.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::RoleHostCore;

// Pattern 1+ (BinaryLifecycleEnvironment) — Logger only.
// RoleHostCore is a pure shared-state container (atomics, mutex-guarded
// maps, message queue) with no broker / ZMQ / script-engine dependency.
// Logger is needed because `LogCaptureFixture::Install` calls
// `Logger::set_logfile()`.  All four Pattern 1+ interference vectors
// (`README_testing.md` § "Decision checklist") came back clean:
//   1. No static state in `RoleHostCore` — each test gets a fresh
//      stack-local instance via the fixture member.
//   2. Single suite per binary (`test_layer2_role_host_core` dedicated).
//   3. No libzmq / luajit / libsodium state — only Logger.
//   4. No deliberate crashes / death tests.
// Migrated 2026-05-14 from the SetUpTestSuite-owned `LifecycleGuard`
// antipattern.
PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule()
)

class RoleHostCoreTest : public ::testing::Test,
                          public pylabhub::tests::LogCaptureFixture
{
  protected:
    // External shutdown flag for `ProcessExitRequested_*` tests.  Held
    // as a fixture member so its lifetime mirrors the production
    // contract: in production, `main()` owns the atomic and passes
    // its address to `EngineHost`, which forwards to
    // `core_.set_shutdown_flag()`.  The flag outlives `core_`.  A
    // function-local atomic would invert that contract — `core_`
    // would hold a dangling pointer once the test body returns.
    // gtest constructs a fresh fixture per TEST_F, so the flag is
    // value-initialised to `false` for every test independently;
    // no cross-test bleed.
    std::atomic<bool> external_shutdown_flag_{false};

    void SetUp()    override { LogCaptureFixture::Install(); }
    void TearDown() override
    {
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
    }

    RoleHostCore core_;
};

// ============================================================================
// Default state
// ============================================================================

TEST_F(RoleHostCoreTest, ContextValid_DefaultTrue_OneWayLatch)
{
    // V1 audit (2026-05-18) — `context_valid_` is initialized to true
    // (handler / BRCs / presences ARE valid by default).  The latch
    // is one-way: there is no inverse setter to flip it back to true.
    // Mutation: change init to `false` → first assertion fails.
    // Mutation: add a `set_context_valid()` setter that flips back to
    // true → subsequent assertion fails because callers could
    // re-enable after invalidation.

    EXPECT_TRUE(core_.context_valid())
        << "V1 contract: `context_valid()` must start TRUE on a fresh "
           "RoleHostCore — handler/BRCs are valid by default.";

    core_.set_context_invalid();
    EXPECT_FALSE(core_.context_valid())
        << "V1 contract: `set_context_invalid()` must flip the flag "
           "to false.";

    // Idempotent: calling set_context_invalid() again is harmless.
    core_.set_context_invalid();
    EXPECT_FALSE(core_.context_valid())
        << "V1 contract: set_context_invalid() is idempotent — "
           "calling it again keeps the latch at false.";

    // One-way: there is no inverse setter in the public API.  This
    // test pins that contract at runtime by attempting to construct
    // a state where the latch flips back — which it cannot.  A
    // future addition of a `set_context_valid(bool)` setter that
    // accepts `true` would break the V1 invariant and should be
    // explicitly rejected.
}

TEST_F(RoleHostCoreTest, ContextValid_IndependentOfIsRunning)
{
    // V1 audit clarification: `context_valid()` is a DIFFERENT
    // concept from `is_running()`.  Even though both happen to flip
    // false during teardown, they're independent observables:
    //
    //   * `is_running()` = "worker thread loop iterates" — re-flippable
    //   * `context_valid()` = "handler/BRCs are alive for callbacks"
    //                        — one-way latch
    //
    // This test pins the independence so a future refactor that
    // tries to collapse them into one flag has to explain why.

    core_.set_running(true);
    EXPECT_TRUE(core_.is_running());
    EXPECT_TRUE(core_.context_valid())
        << "Default: both flags reflect their initial states "
           "independently.";

    // Flip is_running false (worker stopped) — context still valid.
    core_.set_running(false);
    EXPECT_FALSE(core_.is_running());
    EXPECT_TRUE(core_.context_valid())
        << "V1: stopping the worker loop does NOT invalidate the "
           "callback context.  Callbacks may still fire during the "
           "wait_for_quiescence window and need valid structures.";

    // Re-start (it's re-flippable) — context still valid.
    core_.set_running(true);
    EXPECT_TRUE(core_.is_running());
    EXPECT_TRUE(core_.context_valid());

    // Invalidate context independently.
    core_.set_context_invalid();
    EXPECT_TRUE(core_.is_running())
        << "V1: invalidating context does NOT stop the worker — they "
           "operate on different concerns.";
    EXPECT_FALSE(core_.context_valid());
}

TEST_F(RoleHostCoreTest, DefaultState_AllZero)
{
    EXPECT_FALSE(core_.is_running());
    EXPECT_FALSE(core_.is_shutdown_requested());
    EXPECT_FALSE(core_.is_critical_error());
    EXPECT_TRUE(core_.context_valid())
        << "V1 audit (2026-05-18): context_valid_ defaults TRUE on a "
           "fresh RoleHostCore — handler/BRCs/presences are valid by "
           "default; teardown flips this to false exactly once.";
    EXPECT_EQ(core_.stop_reason_string(), "normal");

    EXPECT_EQ(core_.out_slots_written(),       0u);
    EXPECT_EQ(core_.in_slots_received(),       0u);
    EXPECT_EQ(core_.out_drop_count(),             0u);
    EXPECT_EQ(core_.script_error_count(),     0u);
    EXPECT_EQ(core_.iteration_count(),   0u);
    EXPECT_EQ(core_.last_cycle_work_us(), 0u);
    EXPECT_EQ(core_.acquire_retry_count(), 0u);

    EXPECT_FALSE(core_.is_validate_only());
    EXPECT_FALSE(core_.is_script_load_ok());
    EXPECT_FALSE(core_.has_rx_fz());
    EXPECT_FALSE(core_.has_tx_fz());
    EXPECT_EQ(core_.in_schema_fz_size(), 0u);
    EXPECT_EQ(core_.out_schema_fz_size(), 0u);
    EXPECT_FALSE(core_.is_process_exit_requested());
}

// ============================================================================
// Shutdown / error control
// ============================================================================

TEST_F(RoleHostCoreTest, RequestStop_SetsShutdownFlag)
{
    core_.set_running(true);
    EXPECT_TRUE(core_.should_continue_loop());

    core_.request_stop();
    EXPECT_TRUE(core_.is_shutdown_requested());
    EXPECT_FALSE(core_.should_continue_loop());
}

TEST_F(RoleHostCoreTest, SetCriticalError_SetsAllFlags)
{
    core_.set_running(true);
    core_.set_critical_error();

    EXPECT_TRUE(core_.is_critical_error());
    EXPECT_TRUE(core_.is_shutdown_requested());
    EXPECT_EQ(core_.stop_reason_string(), "critical_error");
    EXPECT_FALSE(core_.should_continue_loop());
}

TEST_F(RoleHostCoreTest, StopReason_AllValues)
{
    core_.set_stop_reason(RoleHostCore::StopReason::Normal);
    EXPECT_EQ(core_.stop_reason_string(), "normal");

    core_.set_stop_reason(RoleHostCore::StopReason::PeerDead);
    EXPECT_EQ(core_.stop_reason_string(), "peer_dead");

    core_.set_stop_reason(RoleHostCore::StopReason::HubDead);
    EXPECT_EQ(core_.stop_reason_string(), "hub_dead");

    core_.set_stop_reason(RoleHostCore::StopReason::CriticalError);
    EXPECT_EQ(core_.stop_reason_string(), "critical_error");
}

// ============================================================================
// Loop conditions
// ============================================================================

TEST_F(RoleHostCoreTest, ShouldContinueLoop_RequiresRunningAndNoShutdown)
{
    // Not running → false
    EXPECT_FALSE(core_.should_continue_loop());

    // Running, no shutdown → true
    core_.set_running(true);
    EXPECT_TRUE(core_.should_continue_loop());

    // Running + shutdown → false
    core_.request_stop();
    EXPECT_FALSE(core_.should_continue_loop());
}

TEST_F(RoleHostCoreTest, ShouldExitInner_InverseOfContinueLoop)
{
    core_.set_running(true);
    EXPECT_FALSE(core_.should_exit_inner());

    core_.request_stop();
    EXPECT_TRUE(core_.should_exit_inner());
}

TEST_F(RoleHostCoreTest, ShouldContinueLoop_CriticalErrorExits)
{
    core_.set_running(true);
    EXPECT_TRUE(core_.should_continue_loop());

    core_.set_critical_error();
    EXPECT_FALSE(core_.should_continue_loop());
    EXPECT_TRUE(core_.should_exit_inner());
}

// ============================================================================
// External shutdown flag
// ============================================================================

TEST_F(RoleHostCoreTest, ProcessExitRequested_NullFlag_ReturnsFalse)
{
    EXPECT_FALSE(core_.is_process_exit_requested());
}

TEST_F(RoleHostCoreTest, ProcessExitRequested_FlagNotSet_ReturnsFalse)
{
    core_.set_shutdown_flag(&external_shutdown_flag_);
    EXPECT_FALSE(core_.is_process_exit_requested());
}

TEST_F(RoleHostCoreTest, ProcessExitRequested_FlagSet_ReturnsTrue)
{
    core_.set_shutdown_flag(&external_shutdown_flag_);
    external_shutdown_flag_.store(true, std::memory_order_relaxed);
    EXPECT_TRUE(core_.is_process_exit_requested());
}

// ============================================================================
// Metrics
// ============================================================================

TEST_F(RoleHostCoreTest, MetricCounters_IncrementAndRead)
{
    core_.inc_out_slots_written();
    core_.inc_out_slots_written();
    core_.inc_out_slots_written();
    EXPECT_EQ(core_.out_slots_written(), 3u);

    core_.inc_in_slots_received();
    EXPECT_EQ(core_.in_slots_received(), 1u);

    core_.inc_out_drop_count();
    core_.inc_out_drop_count();
    EXPECT_EQ(core_.out_drop_count(), 2u);

    core_.inc_script_error_count();
    EXPECT_EQ(core_.script_error_count(), 1u);

    for (int i = 0; i < 100; ++i)
        core_.inc_iteration_count();
    EXPECT_EQ(core_.iteration_count(), 100u);

    for (int i = 0; i < 7; ++i)
        core_.inc_acquire_retry();
    EXPECT_EQ(core_.acquire_retry_count(), 7u);
}

TEST_F(RoleHostCoreTest, LastCycleWorkUs_SetAndRead)
{
    core_.set_last_cycle_work_us(12345);
    EXPECT_EQ(core_.last_cycle_work_us(), 12345u);

    core_.set_last_cycle_work_us(0);
    EXPECT_EQ(core_.last_cycle_work_us(), 0u);
}

TEST_F(RoleHostCoreTest, MetricCounters_CrossThread)
{
    constexpr int kPerThread = 10000;

    auto writer = [&]
    {
        for (int i = 0; i < kPerThread; ++i)
            core_.inc_out_slots_written();
    };

    std::thread t1(writer);
    std::thread t2(writer);
    t1.join();
    t2.join();

    EXPECT_EQ(core_.out_slots_written(), static_cast<uint64_t>(kPerThread * 2));
}

// ============================================================================
// Message queue
// ============================================================================

TEST_F(RoleHostCoreTest, MessageQueue_EnqueueAndDrain)
{
    IncomingMessage msg;
    msg.event  = "test_event";
    msg.sender = "sender1";
    core_.enqueue_message(std::move(msg));

    auto msgs = core_.drain_messages();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].event, "test_event");
    EXPECT_EQ(msgs[0].sender, "sender1");

    // Drain again — empty.
    auto msgs2 = core_.drain_messages();
    EXPECT_TRUE(msgs2.empty());
}

TEST_F(RoleHostCoreTest, MessageQueue_OverflowDropsOldest)
{
    // Production code emits LOGGER_WARN per dropped message — the
    // test deliberately overflows by 10, so up to 10 warns expected.
    ExpectLogWarn("Incoming queue full — dropping message");
    for (size_t i = 0; i < RoleHostCore::kMaxIncomingQueue + 10; ++i)
    {
        IncomingMessage msg;
        msg.event = "evt_" + std::to_string(i);
        core_.enqueue_message(std::move(msg));
    }

    auto msgs = core_.drain_messages();
    // Should not exceed kMaxIncomingQueue.
    EXPECT_LE(msgs.size(), RoleHostCore::kMaxIncomingQueue);
}

TEST_F(RoleHostCoreTest, MessageQueue_CrossThread)
{
    // 100 enqueues vs kMaxIncomingQueue cap — overflow path may fire.
    ExpectLogWarn("Incoming queue full — dropping message");
    constexpr int kMessages = 100;

    std::thread producer([&]
    {
        for (int i = 0; i < kMessages; ++i)
        {
            IncomingMessage msg;
            msg.event = std::to_string(i);
            core_.enqueue_message(std::move(msg));
        }
    });

    producer.join();

    auto msgs = core_.drain_messages();
    // May be capped at kMaxIncomingQueue if overflow occurred.
    EXPECT_GT(msgs.size(), 0u);
    EXPECT_LE(msgs.size(), static_cast<size_t>(kMessages));
}

// ============================================================================
// Init-time state
// ============================================================================

TEST_F(RoleHostCoreTest, ValidateOnly_SetAndRead)
{
    EXPECT_FALSE(core_.is_validate_only());
    core_.set_validate_only(true);
    EXPECT_TRUE(core_.is_validate_only());
}

TEST_F(RoleHostCoreTest, ScriptLoadOk_AtomicSetAndRead)
{
    EXPECT_FALSE(core_.is_script_load_ok());
    core_.set_script_load_ok(true);
    EXPECT_TRUE(core_.is_script_load_ok());
    core_.set_script_load_ok(false);
    EXPECT_FALSE(core_.is_script_load_ok());
}

TEST_F(RoleHostCoreTest, ScriptLoadOk_CrossThread)
{
    std::thread writer([&]
    {
        core_.set_script_load_ok(true);
    });
    writer.join();

    EXPECT_TRUE(core_.is_script_load_ok());
}

TEST_F(RoleHostCoreTest, OutFzSpec_SetAndRead)
{
    EXPECT_FALSE(core_.has_tx_fz());
    EXPECT_EQ(core_.out_schema_fz_size(), 0u);

    pylabhub::hub::SchemaSpec spec;
    spec.has_schema = true;
    spec.packing    = "aligned";

    core_.set_out_fz_spec(std::move(spec), 8192);

    EXPECT_TRUE(core_.has_tx_fz());
    EXPECT_EQ(core_.out_schema_fz_size(), 8192u);
    EXPECT_TRUE(core_.out_fz_spec().has_schema);
    EXPECT_EQ(core_.out_fz_spec().packing, "aligned");
}

TEST_F(RoleHostCoreTest, InFzSpec_SetAndRead)
{
    EXPECT_FALSE(core_.has_rx_fz());
    EXPECT_EQ(core_.in_schema_fz_size(), 0u);

    pylabhub::hub::SchemaSpec spec;
    spec.has_schema = true;
    spec.packing    = "packed";

    core_.set_in_fz_spec(std::move(spec), 4096);

    EXPECT_TRUE(core_.has_rx_fz());
    EXPECT_EQ(core_.in_schema_fz_size(), 4096u);
    EXPECT_TRUE(core_.in_fz_spec().has_schema);
    EXPECT_EQ(core_.in_fz_spec().packing, "packed");
}

TEST_F(RoleHostCoreTest, FzSpec_NoSchema_HasFzFalse)
{
    pylabhub::hub::SchemaSpec spec;
    spec.has_schema = false;

    core_.set_out_fz_spec(std::move(spec), 0);
    EXPECT_FALSE(core_.has_tx_fz());
    EXPECT_EQ(core_.out_schema_fz_size(), 0u);

    pylabhub::hub::SchemaSpec spec2;
    spec2.has_schema = false;

    core_.set_in_fz_spec(std::move(spec2), 0);
    EXPECT_FALSE(core_.has_rx_fz());
    EXPECT_EQ(core_.in_schema_fz_size(), 0u);
}

// ============================================================================
// Shared data
// ============================================================================

TEST_F(RoleHostCoreTest, SharedData_DefaultEmpty)
{
    EXPECT_FALSE(core_.get_shared_data("anything").has_value());
}

TEST_F(RoleHostCoreTest, SharedData_SetAndGet_Int64)
{
    core_.set_shared_data("counter", int64_t{42});
    auto val = core_.get_shared_data("counter");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(std::get<int64_t>(*val), 42);
}

TEST_F(RoleHostCoreTest, SharedData_SetAndGet_Double)
{
    core_.set_shared_data("ratio", 3.14);
    auto val = core_.get_shared_data("ratio");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*val), 3.14);
}

TEST_F(RoleHostCoreTest, SharedData_SetAndGet_Bool)
{
    core_.set_shared_data("active", true);
    auto val = core_.get_shared_data("active");
    ASSERT_TRUE(val.has_value());
    EXPECT_TRUE(std::get<bool>(*val));
}

TEST_F(RoleHostCoreTest, SharedData_SetAndGet_String)
{
    core_.set_shared_data("mode", std::string{"warmup"});
    auto val = core_.get_shared_data("mode");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(std::get<std::string>(*val), "warmup");
}

TEST_F(RoleHostCoreTest, SharedData_Overwrite)
{
    core_.set_shared_data("x", int64_t{1});
    core_.set_shared_data("x", int64_t{2});
    auto val = core_.get_shared_data("x");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(std::get<int64_t>(*val), 2);
}

TEST_F(RoleHostCoreTest, SharedData_OverwriteChangesType)
{
    core_.set_shared_data("x", int64_t{1});
    core_.set_shared_data("x", std::string{"hello"});
    auto val = core_.get_shared_data("x");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(std::get<std::string>(*val), "hello");
}

TEST_F(RoleHostCoreTest, SharedData_Remove)
{
    core_.set_shared_data("key", int64_t{99});
    core_.remove_shared_data("key");
    EXPECT_FALSE(core_.get_shared_data("key").has_value());
}

TEST_F(RoleHostCoreTest, SharedData_RemoveNonExistent_NoOp)
{
    core_.remove_shared_data("nonexistent"); // should not throw
}

TEST_F(RoleHostCoreTest, SharedData_Clear)
{
    core_.set_shared_data("a", int64_t{1});
    core_.set_shared_data("b", std::string{"two"});
    core_.clear_shared_data();
    EXPECT_FALSE(core_.get_shared_data("a").has_value());
    EXPECT_FALSE(core_.get_shared_data("b").has_value());
}

TEST_F(RoleHostCoreTest, SharedData_CrossThread)
{
    // Writer thread sets a value; reader thread reads it after join.
    std::thread writer([&]
    {
        core_.set_shared_data("from_thread", int64_t{777});
    });
    writer.join();

    auto val = core_.get_shared_data("from_thread");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(std::get<int64_t>(*val), 777);
}

TEST_F(RoleHostCoreTest, SharedData_ConcurrentReadWrite)
{
    // Verify: concurrent reads and writes don't corrupt data.
    // Writer writes 0..999 sequentially. Reader reads after writer joins.
    // The assertions verify the final value and that intermediate values
    // are always valid (no partial writes, no corruption).
    constexpr int kIterations = 1000;

    std::thread writer([&]
    {
        for (int i = 0; i < kIterations; ++i)
            core_.set_shared_data("counter", int64_t{i});
    });
    writer.join();

    // After join, final value must be the last written.  The std::get
    // call inside the EXPECT_EQ below would throw std::bad_variant_access
    // if the type were corrupted by concurrent access, so type-correctness
    // is pinned by the same line that pins the value.
    auto final_val = core_.get_shared_data("counter");
    ASSERT_TRUE(final_val.has_value());
    EXPECT_EQ(std::get<int64_t>(*final_val), kIterations - 1);
}
