#include "tests/logger/test_fixture.h"

TEST_F(LoggerTest, ShutdownIdempotency)
{
    auto log_path = GetUniqueLogPath("shutdown_idempotency");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_shutdown_idempotency",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}
