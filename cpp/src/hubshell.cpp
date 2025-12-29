#include "utils/Lifecycle.hpp"

int main()
{
    LifecycleManager::instance().initialize();

    // Main application logic will go here.

    LifecycleManager::instance().finalize();
    return 0;
}