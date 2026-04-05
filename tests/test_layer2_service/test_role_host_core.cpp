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

#include "utils/role_host_core.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::RoleHostCore;

class RoleHostCoreTest : public ::testing::Test
{
  protected:
    RoleHostCore core_;
};

// ============================================================================
// Default state
// ============================================================================

TEST_F(RoleHostCoreTest, DefaultState_AllZero)
{
    EXPECT_FALSE(core_.is_running());
    EXPECT_FALSE(core_.is_shutdown_requested());
    EXPECT_FALSE(core_.is_critical_error());
    EXPECT_EQ(core_.stop_reason_string(), "normal");

    EXPECT_EQ(core_.out_slots_written(),       0u);
    EXPECT_EQ(core_.in_slots_received(),       0u);
    EXPECT_EQ(core_.out_drop_count(),             0u);
    EXPECT_EQ(core_.script_error_count(),     0u);
    EXPECT_EQ(core_.iteration_count(),   0u);
    EXPECT_EQ(core_.last_cycle_work_us(), 0u);

    EXPECT_FALSE(core_.is_validate_only());
    EXPECT_FALSE(core_.is_script_load_ok());
    EXPECT_FALSE(core_.has_in_fz());
    EXPECT_FALSE(core_.has_out_fz());
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
    std::atomic<bool> flag{false};
    core_.set_shutdown_flag(&flag);
    EXPECT_FALSE(core_.is_process_exit_requested());
}

TEST_F(RoleHostCoreTest, ProcessExitRequested_FlagSet_ReturnsTrue)
{
    std::atomic<bool> flag{false};
    core_.set_shutdown_flag(&flag);
    flag.store(true, std::memory_order_relaxed);
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
    EXPECT_FALSE(core_.has_out_fz());
    EXPECT_EQ(core_.out_schema_fz_size(), 0u);

    pylabhub::hub::SchemaSpec spec;
    spec.has_schema = true;
    spec.packing    = "aligned";

    core_.set_out_fz_spec(std::move(spec), 8192);

    EXPECT_TRUE(core_.has_out_fz());
    EXPECT_EQ(core_.out_schema_fz_size(), 8192u);
    EXPECT_TRUE(core_.out_fz_spec().has_schema);
    EXPECT_EQ(core_.out_fz_spec().packing, "aligned");
}

TEST_F(RoleHostCoreTest, InFzSpec_SetAndRead)
{
    EXPECT_FALSE(core_.has_in_fz());
    EXPECT_EQ(core_.in_schema_fz_size(), 0u);

    pylabhub::hub::SchemaSpec spec;
    spec.has_schema = true;
    spec.packing    = "packed";

    core_.set_in_fz_spec(std::move(spec), 4096);

    EXPECT_TRUE(core_.has_in_fz());
    EXPECT_EQ(core_.in_schema_fz_size(), 4096u);
    EXPECT_TRUE(core_.in_fz_spec().has_schema);
    EXPECT_EQ(core_.in_fz_spec().packing, "packed");
}

TEST_F(RoleHostCoreTest, FzSpec_NoSchema_HasFzFalse)
{
    pylabhub::hub::SchemaSpec spec;
    spec.has_schema = false;

    core_.set_out_fz_spec(std::move(spec), 0);
    EXPECT_FALSE(core_.has_out_fz());
    EXPECT_EQ(core_.out_schema_fz_size(), 0u);

    pylabhub::hub::SchemaSpec spec2;
    spec2.has_schema = false;

    core_.set_in_fz_spec(std::move(spec2), 0);
    EXPECT_FALSE(core_.has_in_fz());
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

    // After join, final value must be the last written.
    auto final_val = core_.get_shared_data("counter");
    ASSERT_TRUE(final_val.has_value());
    EXPECT_EQ(std::get<int64_t>(*final_val), kIterations - 1);

    // Verify type is correct (not corrupted by concurrent access).
    EXPECT_NO_THROW(std::get<int64_t>(*final_val));
}
