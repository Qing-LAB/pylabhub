#include "tests/logger/test_fixture.h"

TEST_F(LoggerTest, DISABLED_PlatformSinks)
{
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_platform_sinks", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}
