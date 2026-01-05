#include "platform.hpp"
#include "utils/FileLock.hpp"
#include "utils/JsonConfig.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

using namespace pylabhub::utils;
int main()
{
    LifecycleGuard app_lifecycle(pylabhub::utils::Logger::GetLifecycleModule(),
                                 pylabhub::utils::FileLock::GetLifecycleModule(),
                                 pylabhub::utils::JsonConfig::GetLifecycleModule());

    // Main application logic will go here.

    return 0;
}