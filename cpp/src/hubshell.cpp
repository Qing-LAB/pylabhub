#include "plh_datahub.hpp"

using namespace pylabhub::utils;
int main()
{
    LifecycleGuard app_lifecycle(MakeModDefList(pylabhub::utils::Logger::GetLifecycleModule(),
                                          pylabhub::utils::FileLock::GetLifecycleModule(),
                                          pylabhub::utils::JsonConfig::GetLifecycleModule()));

    // Main application logic will go here.

    return 0;
}