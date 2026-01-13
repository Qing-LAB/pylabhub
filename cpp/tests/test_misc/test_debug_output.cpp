// tests/test_misc/test_debug_output.cpp
#include "debug_info.hpp"
#include "platform.hpp"
#include <iostream>
#include <string>

using namespace pylabhub::debug;

void wrapper_function(int x)
{
    // This will capture wrapper location (since wrapper doesn't forward loc).
    debug_msg("calling debug_msg() within a function : x = {}, called from {}", x,
              PLH_LOC_HERE_STR);
}

void test_rt_runtime_format()
{
    std::string runtime_fmt = "rt fmt value = {:.2f}";
    debug_msg_rt("rt fmt value = {:.2f}", 3.14159); // OK: const Args&... binds to temporary
    std::cout << "here is the stack trace:\n";
    print_stack_trace();
}

int main()
{
    std::cout << "=== Direct calls ===\n";
    debug_msg("Hello from main: {} + {} = {}, called from {}", 2, 3, 5, PLH_LOC_HERE_STR); // OK
    debug_msg("A simple literal message");                                                 // OK
    debug_msg("one arg: {}", 42);                                                          // OK

    std::cout << "\n=== debug msg from a function ===\n";
    wrapper_function(42);

    std::cout << "\n=== runtime fmt (rt) ===\n";
    test_rt_runtime_format();

    std::cout << "\n=== macro usage (optional) ===\n";
    PLH_DEBUG("Using PLH_DEBUG macro: value = {}, called from {}", 42, PLH_LOC_HERE_STR);

    std::cout << "\n=== test will panic ===\n";
    PLH_PANIC("This is a panic message with code {}", -1);

    return 0;
}
