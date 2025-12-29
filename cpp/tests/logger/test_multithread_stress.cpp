#include "test_preamble.h" // New common preamble

#include "test_fixture.h" // Keep this specific fixture header

TEST_F(LoggerTest, MultithreadStress)
{
    auto log_path = GetUniqueLogPath("multithread_stress");
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "logger.test_multithread_stress", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}
