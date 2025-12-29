#include "utils/Lifecycle.hpp"

int main()
{
    pylabhub_initialize_application();

    // Main application logic will go here.

    pylabhub_finalize_application();
    return 0;
}