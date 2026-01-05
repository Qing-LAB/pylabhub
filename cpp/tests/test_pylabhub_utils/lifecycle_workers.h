#pragma once

namespace pylabhub::tests::worker::lifecycle
{

int test_multiple_guards_warning();
int test_module_registration_and_initialization();
int test_is_initialized_flag();
int test_register_after_init_aborts();

} // namespace pylabhub::tests::worker::lifecycle
