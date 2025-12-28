#include "tests/logger/test_fixture.h"

TEST_F(LoggerTest, FlushWaitsForQueue)
{
    auto log_path = GetUniqueLogPath("flush_waits_for_queue");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_flush_waits_for_queue",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}
