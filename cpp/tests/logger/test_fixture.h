#include "test_preamble.h" // New common preamble

#include "../helpers/test_entrypoint.h"   // Keep this specific header
#include "../helpers/test_process_utils.h" // Keep this specific header
using namespace test_utils;

class LoggerTest : public ::testing::Test
{
protected:
    std::vector<fs::path> paths_to_clean_;

    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            try
            {
                if (fs::exists(p)) fs::remove(p);
            }
            catch (...)
            {
                // best-effort cleanup
            }
        }
    }

    fs::path GetUniqueLogPath(const std::string &test_name)
    {
        auto p = fs::temp_directory_path() / ("pylabhub_test_" + test_name + ".log");
        paths_to_clean_.push_back(p);
        // Ensure the file does not exist from a previous failed run.
        try
        {
            if (fs::exists(p)) fs::remove(p);
        }
        catch (...)
        {
        }
        return p;
    }
};
