#include "test_entrypoint.h"

// Project-specific Worker Headers
#include "filelock_workers.h"
#include "fmt/core.h"
#include "jsonconfig_workers.h"
#include "lifecycle_workers.h"
#include "logger_workers.h"

#include <string>
#include <vector>

// This is the implementation of the worker dispatcher for the pylabhub_utils tests.
// It is responsible for parsing the worker mode argument and calling the correct
// worker function.
static int dispatch_utils_workers(int argc, char **argv)
{
    if (argc < 2)
        return -1;

    std::string mode_str = argv[1];
    size_t dot_pos = mode_str.find('.');
    if (dot_pos == std::string::npos)
        return -1;

    std::string module = mode_str.substr(0, dot_pos);
    std::string scenario = mode_str.substr(dot_pos + 1);

    // Dispatch to the appropriate worker function based on the module and scenario.
    // The return value of the worker function becomes the exit code of the process.
    if (module == "filelock")
    {
        fmt::print("Dispatching to filelock worker scenario: '{}'\n", scenario);
        if (scenario == "nonblocking_acquire" && argc > 2)
        {
            return pylabhub::tests::worker::filelock::nonblocking_acquire(argv[2]);
        }
        if (scenario == "contention_log_access" && argc > 4)
        {
            return pylabhub::tests::worker::filelock::contention_log_access(argv[2], argv[3],
                                                                            std::stoi(argv[4]));
        }
        if (scenario == "parent_child_block" && argc > 2)
        {
            return pylabhub::tests::worker::filelock::parent_child_block(argv[2]);
        }
        if (scenario == "test_basic_non_blocking" && argc > 2)
        {
            return pylabhub::tests::worker::filelock::test_basic_non_blocking(argv[2]);
        }
        if (scenario == "test_blocking_lock" && argc > 2)
        {
            return pylabhub::tests::worker::filelock::test_blocking_lock(argv[2]);
        }
        if (scenario == "test_timed_lock" && argc > 2)
        {
            return pylabhub::tests::worker::filelock::test_timed_lock(argv[2]);
        }
        if (scenario == "test_move_semantics" && argc > 3)
        {
            return pylabhub::tests::worker::filelock::test_move_semantics(argv[2], argv[3]);
        }
        if (scenario == "test_directory_creation" && argc > 2)
        {
            return pylabhub::tests::worker::filelock::test_directory_creation(argv[2]);
        }
        if (scenario == "test_directory_path_locking" && argc > 2)
        {
            return pylabhub::tests::worker::filelock::test_directory_path_locking(argv[2]);
        }
        if (scenario == "test_multithreaded_non_blocking" && argc > 2)
        {
            return pylabhub::tests::worker::filelock::test_multithreaded_non_blocking(argv[2]);
        }
        if (scenario == "try_lock_nonblocking" && argc > 2)
        {
            return pylabhub::tests::worker::filelock::try_lock_nonblocking(argv[2]);
        }
        fmt::print(stderr, "ERROR: Unknown scenario '{}' for module '{}'\n", scenario, module);
        return 1;
    }
    else if (module == "jsonconfig")
    {
        fmt::print("Dispatching to jsonconfig worker scenario: '{}'\n", scenario);
        if (scenario == "write_id" && argc > 3)
        {
            return pylabhub::tests::worker::jsonconfig::write_id(argv[2], argv[3]);
        }
        if (scenario == "uninitialized_behavior")
        {
            return pylabhub::tests::worker::jsonconfig::uninitialized_behavior();
        }
        if (scenario == "not_consuming_proxy")
        {
            return pylabhub::tests::worker::jsonconfig::not_consuming_proxy();
        }
        fmt::print(stderr, "ERROR: Unknown scenario '{}' for module '{}'\n", scenario, module);
        return 1;
    }
    else if (module == "lifecycle")
    {
        fmt::print("Dispatching to lifecycle worker scenario: '{}'\n", scenario);
        // --- Static Lifecycle Tests ---
        if (scenario == "test_multiple_guards_warning")
        {
            return pylabhub::tests::worker::lifecycle::test_multiple_guards_warning();
        }
        if (scenario == "test_module_registration_and_initialization")
        {
            return pylabhub::tests::worker::lifecycle::
                test_module_registration_and_initialization();
        }
        if (scenario == "test_is_initialized_flag")
        {
            return pylabhub::tests::worker::lifecycle::test_is_initialized_flag();
        }
        if (scenario == "test_register_after_init_aborts")
        {
            return pylabhub::tests::worker::lifecycle::test_register_after_init_aborts();
        }
        if (scenario == "test_unresolved_dependency")
        {
            return pylabhub::tests::worker::lifecycle::test_unresolved_dependency();
        }
        if (scenario == "test_case_insensitive_dependency")
        {
            return pylabhub::tests::worker::lifecycle::test_case_insensitive_dependency();
        }
        if (scenario == "test_static_circular_dependency_aborts")
        {
            return pylabhub::tests::worker::lifecycle::test_static_circular_dependency_aborts();
        }
        if (scenario == "test_static_elaborate_indirect_cycle_aborts")
        {
            return pylabhub::tests::worker::lifecycle::
                test_static_elaborate_indirect_cycle_aborts();
        }
        // --- Dynamic Lifecycle Tests ---
        if (scenario == "dynamic.load_unload")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_load_unload();
        }
        if (scenario == "dynamic.ref_counting")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_ref_counting();
        }
        if (scenario == "dynamic.dependency_chain")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_dependency_chain();
        }
        if (scenario == "dynamic.diamond_dependency")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_diamond_dependency();
        }
        if (scenario == "dynamic.finalize_unloads_all")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_finalize_unloads_all();
        }
        if (scenario == "dynamic.persistent_in_middle")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_persistent_in_middle();
        }
        if (scenario == "dynamic.static_dependency_fail")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_static_dependency_fail();
        }
        if (scenario == "registration_fails_with_unresolved_dependency")
        {
            return pylabhub::tests::worker::lifecycle::
                registration_fails_with_unresolved_dependency();
        }
        if (scenario == "dynamic.reentrant_load_fail")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_reentrant_load_fail();
        }
        if (scenario == "dynamic.register_before_init_fail")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_register_before_init_fail();
        }
        if (scenario == "dynamic.persistent_module")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_persistent_module();
        }
        if (scenario == "dynamic.persistent_module_finalize")
        {
            return pylabhub::tests::worker::lifecycle::dynamic_persistent_module_finalize();
        }
        fmt::print(stderr, "ERROR: Unknown scenario '{}' for module '{}'\n", scenario, module);
        return 1;
    }
    else if (module == "logger")
    {
        fmt::print("Dispatching to logger worker scenario: '{}'\n", scenario);
        if (scenario == "test_basic_logging" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_basic_logging(argv[2]);
        }
        if (scenario == "test_log_level_filtering" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_log_level_filtering(argv[2]);
        }
        if (scenario == "test_bad_format_string" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_bad_format_string(argv[2]);
        }
        if (scenario == "test_default_sink_and_switching" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_default_sink_and_switching(argv[2]);
        }
        if (scenario == "test_multithread_stress" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_multithread_stress(argv[2]);
        }
        if (scenario == "test_flush_waits_for_queue" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_flush_waits_for_queue(argv[2]);
        }
        if (scenario == "test_shutdown_idempotency" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_shutdown_idempotency(argv[2]);
        }
        if (scenario == "test_reentrant_error_callback" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_reentrant_error_callback(argv[2]);
        }
        if (scenario == "test_write_error_callback_async" && argc > 1)
        {
            return pylabhub::tests::worker::logger::test_write_error_callback_async();
        }
        if (scenario == "test_platform_sinks" && argc > 1)
        {
            return pylabhub::tests::worker::logger::test_platform_sinks();
        }
        if (scenario == "test_concurrent_lifecycle_chaos" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_concurrent_lifecycle_chaos(argv[2]);
        }
        if (scenario == "stress_log" && argc > 3)
        {
            pylabhub::tests::worker::logger::stress_log(argv[2], std::stoi(argv[3]));
            return 0;
        }
        if (scenario == "test_inter_process_flock" && argc > 4)
        {
            return pylabhub::tests::worker::logger::test_inter_process_flock(argv[2], argv[3],
                                                                             std::stoi(argv[4]));
        }
        if (scenario == "test_rotating_file_sink" && argc > 4)
        {
            return pylabhub::tests::worker::logger::test_rotating_file_sink(
                argv[2], static_cast<size_t>(std::stoul(argv[3])),
                static_cast<size_t>(std::stoul(argv[4])));
        }
        if (scenario == "test_queue_full_and_message_dropping" && argc > 2)
        {
            return pylabhub::tests::worker::logger::test_queue_full_and_message_dropping(argv[2]);
        }
        fmt::print(stderr, "ERROR: Unknown scenario '{}' for module '{}'\n", scenario, module);
        return 1;
    }
    return -1; // No matching worker found
}

// Static registrar object. The constructor registers our dispatcher with the framework.
struct WorkerRegistrar
{
    WorkerRegistrar() { register_worker_dispatcher(dispatch_utils_workers); }
};
static WorkerRegistrar g_registrar;
