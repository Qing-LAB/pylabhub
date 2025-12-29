#include "test_preamble.h" // New common preamble

#include "test_fixture.h" // Keep this specific fixture header

TEST_F(LoggerTest, ReentrantErrorCallback)
{
    auto log_path = GetUniqueLogPath("reentrant_error_callback");
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "logger.test_reentrant_error_callback", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}
