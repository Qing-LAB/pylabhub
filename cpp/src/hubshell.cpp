#include "plh_datahub.hpp"

using namespace pylabhub::utils;

int main()
{
    // LifecycleGuard ensures modules start in dependency order and shut down in reverse order.
    // Include all modules your application uses: Logger, FileLock, CryptoUtils (for DataBlock/MessageHub),
    // Data Exchange Hub (MessageHub + DataBlock), JsonConfig if needed.
    LifecycleGuard app_lifecycle(MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule()));

    // Main application logic will go here.

    return 0;
}