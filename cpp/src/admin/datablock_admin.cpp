#include <iostream>
#include <string>
#include <vector>
#include "utils/slot_diagnostics.hpp"
#include "utils/slot_recovery.hpp"
#include "utils/integrity_validator.hpp"
#include "utils/recovery_api.hpp"

void print_usage()
{
    std::cerr
        << "Usage: datablock-admin <command> [<args>]
           "
        << "Commands:
           "
        << "  diagnose <shm_name> [--slot <slot_index>]   Display diagnostic info.
           "
        << "  recover <shm_name> --slot <slot_index> --action <action> [--force]
           "
        << "                                            Perform a recovery action.
           "
        << "                                            Actions: force_reset, release_readers, release_writer
           "
        << "  cleanup <shm_name>                          Cleanup dead consumers.
           "
        << "  validate <shm_name> [--repair]              Validate integrity.
           "
        << std::endl;
}

void print_diagnostics(const pylabhub::hub::SlotDiagnostic &diag)
{
    std::cout << "Slot " << diag.slot_index
              << " Diagnostics:
                 "
              << "  Slot ID: " << diag.slot_id
              << "
                 "
              << "  State: " << static_cast<int>(diag.slot_state)
              << "
                 "
              << "  Write Lock PID: " << diag.write_lock
              << "
                 "
              << "  Reader Count: " << diag.reader_count
              << "
                 "
              << "  Is Stuck: " << (diag.is_stuck ? "Yes" : "No")
              << "
                 "
              << "  Stuck Duration (ms): " << diag.stuck_duration_ms
              << "
                 "
              << std::endl;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    if (command == "diagnose")
    {
        if (argc < 3)
        {
            print_usage();
            return 1;
        }
        std::string shm_name = argv[2];
        if (argc > 3 && std::string(argv[3]) == "--slot")
        {
            if (argc < 5)
            {
                print_usage();
                return 1;
            }
            uint32_t slot_index = std::stoul(argv[4]);
            pylabhub::hub::SlotDiagnostics diagnostics(shm_name, slot_index);
            if (diagnostics.is_stuck())
            {   // This is a placeholder for a better check
                // For now, just print the raw data.
            }
            SlotDiagnostic diag_data;
            if (datablock_diagnose_slot(shm_name.c_str(), slot_index, &diag_data) == 0)
            {
                print_diagnostics(diag_data);
            }
            else
            {
                std::cerr << "Failed to diagnose slot " << slot_index << " for " << shm_name
                          << std::endl;
                return 1;
            }
        }
        else
        {
            // Diagnose all slots
            std::vector<SlotDiagnostic> diags(128); // Max 128 slots for now
            size_t count = 0;
            if (datablock_diagnose_all_slots(shm_name.c_str(), diags.data(), diags.size(),
                                             &count) == 0)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    print_diagnostics(diags[i]);
                }
            }
            else
            {
                std::cerr << "Failed to diagnose all slots for " << shm_name << std::endl;
                return 1;
            }
        }
    }
    else if (command == "recover")
    {
        if (argc < 6)
        {
            print_usage();
            return 1;
        }
        std::string shm_name = argv[2];
        uint32_t slot_index = 0;
        std::string action;
        bool force = false;

        for (int i = 3; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--slot")
            {
                if (i + 1 < argc)
                    slot_index = std::stoul(argv[++i]);
            }
            else if (arg == "--action")
            {
                if (i + 1 < argc)
                    action = argv[++i];
            }
            else if (arg == "--force")
            {
                force = true;
            }
        }

        pylabhub::hub::SlotRecovery recovery(shm_name, slot_index);
        RecoveryResult result;
        if (action == "force_reset")
        {
            result = recovery.force_reset(force);
        }
        else if (action == "release_readers")
        {
            result = recovery.release_zombie_readers(force);
        }
        else if (action == "release_writer")
        {
            result = recovery.release_zombie_writer();
        }
        else
        {
            std::cerr << "Unknown recovery action: " << action << std::endl;
            return 1;
        }

        if (result == RECOVERY_SUCCESS)
        {
            std::cout << "Recovery action '" << action << "' completed successfully." << std::endl;
        }
        else
        {
            std::cerr << "Recovery action '" << action << "' failed with code " << result
                      << std::endl;
            return 1;
        }
    }
    else if (command == "cleanup")
    {
        if (argc < 3)
        {
            print_usage();
            return 1;
        }
        std::string shm_name = argv[2];
        if (datablock_cleanup_dead_consumers(shm_name.c_str()) == RECOVERY_SUCCESS)
        {
            std::cout << "Cleanup of dead consumers completed successfully." << std::endl;
        }
        else
        {
            std::cerr << "Cleanup of dead consumers failed." << std::endl;
            return 1;
        }
    }
    else if (command == "validate")
    {
        if (argc < 3)
        {
            print_usage();
            return 1;
        }
        std::string shm_name = argv[2];
        bool repair = false;
        if (argc > 3 && std::string(argv[3]) == "--repair")
        {
            repair = true;
        }
        pylabhub::hub::IntegrityValidator validator(shm_name);
        if (validator.validate(repair) == RECOVERY_SUCCESS)
        {
            std::cout << "Integrity validation completed successfully." << std::endl;
        }
        else
        {
            std::cerr << "Integrity validation failed." << std::endl;
            return 1;
        }
    }
    else
    {
        std::cerr << "Unknown command: " << command << std::endl;
        print_usage();
        return 1;
    }

    return 0;
}
