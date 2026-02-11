import ctypes
import os
import sys
from enum import IntEnum

# This assumes the script is run from the `build` directory
# or that the library is in a location discoverable by the system.
lib_name = 'libpylabhub-utils-stable'
if sys.platform == 'win32':
    lib_name = 'pylabhub-utils-stable.dll'
elif sys.platform == 'darwin':
    lib_name += '.dylib'
else:
    lib_name += '.so'

try:
    # Try loading from a path relative to this script, assuming it's in build/lib
    lib_path = os.path.abspath(os.path.join(os.path.dirname(__file__), lib_name))
    pylabhub_lib = ctypes.CDLL(lib_path)
except OSError:
    try:
        # Fallback to system-wide search
        pylabhub_lib = ctypes.CDLL(lib_name)
    except OSError as e:
        print(f"Error: Could not load the pylabhub library. Ensure it is built and discoverable.")
        print(f"Attempted to load '{lib_name}' and from path '{lib_path}'")
        print(f"Original error: {e}")
        sys.exit(1)


class SlotState(IntEnum):
    FREE = 0
    WRITING = 1
    COMMITTED = 2
    DRAINING = 3

class RecoveryResult(IntEnum):
    RECOVERY_SUCCESS = 0
    RECOVERY_FAILED = 1
    RECOVERY_UNSAFE = 2
    RECOVERY_NOT_STUCK = 3
    RECOVERY_INVALID_SLOT = 4

class SlotDiagnostic(ctypes.Structure):
    _fields_ = [
        ("slot_id", ctypes.c_uint64),
        ("slot_index", ctypes.c_uint32),
        ("slot_state", ctypes.c_uint8),
        ("write_lock", ctypes.c_uint64),
        ("reader_count", ctypes.c_uint32),
        ("write_generation", ctypes.c_uint64),
        ("writer_waiting", ctypes.c_uint8),
        ("is_stuck", ctypes.c_bool),
        ("stuck_duration_ms", ctypes.c_uint64),
    ]

    def __repr__(self):
        return (
            f"SlotDiagnostic(slot_index={self.slot_index}, state={SlotState(self.slot_state).name}, "
            f"write_lock={self.write_lock}, readers={self.reader_count}, stuck={self.is_stuck})"
        )

# --- Function Prototypes ---
pylabhub_lib.datablock_diagnose_slot.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.POINTER(SlotDiagnostic)]
pylabhub_lib.datablock_diagnose_slot.restype = ctypes.c_int

pylabhub_lib.datablock_diagnose_all_slots.argtypes = [ctypes.c_char_p, ctypes.POINTER(SlotDiagnostic), ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
pylabhub_lib.datablock_diagnose_all_slots.restype = ctypes.c_int

pylabhub_lib.datablock_is_process_alive.argtypes = [ctypes.c_uint64]
pylabhub_lib.datablock_is_process_alive.restype = ctypes.c_bool

pylabhub_lib.datablock_force_reset_slot.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_bool]
pylabhub_lib.datablock_force_reset_slot.restype = RecoveryResult

pylabhub_lib.datablock_force_reset_all_slots.argtypes = [ctypes.c_char_p, ctypes.c_bool]
pylabhub_lib.datablock_force_reset_all_slots.restype = RecoveryResult

pylabhub_lib.datablock_release_zombie_readers.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_bool]
pylabhub_lib.datablock_release_zombie_readers.restype = RecoveryResult

pylabhub_lib.datablock_release_zombie_writer.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
pylabhub_lib.datablock_release_zombie_writer.restype = RecoveryResult

pylabhub_lib.datablock_cleanup_dead_consumers.argtypes = [ctypes.c_char_p]
pylabhub_lib.datablock_cleanup_dead_consumers.restype = RecoveryResult

pylabhub_lib.datablock_validate_integrity.argtypes = [ctypes.c_char_p, ctypes.c_bool]
pylabhub_lib.datablock_validate_integrity.restype = RecoveryResult

# --- Pythonic Wrappers ---

def diagnose_slot(shm_name: str, slot_index: int) -> SlotDiagnostic:
    diag = SlotDiagnostic()
    result = pylabhub_lib.datablock_diagnose_slot(shm_name.encode('utf-8'), slot_index, ctypes.byref(diag))
    if result != 0:
        raise RuntimeError(f"Failed to diagnose slot {slot_index} for {shm_name}. Error code: {result}")
    return diag

def diagnose_all_slots(shm_name: str, max_slots: int = 128) -> list[SlotDiagnostic]:
    diags_array = (SlotDiagnostic * max_slots)()
    count = ctypes.c_size_t(0)
    result = pylabhub_lib.datablock_diagnose_all_slots(shm_name.encode('utf-8'), diags_array, max_slots, ctypes.byref(count))
    if result != 0:
        raise RuntimeError(f"Failed to diagnose all slots for {shm_name}. Error code: {result}")
    return list(diags_array[:count.value])

def is_process_alive(pid: int) -> bool:
    return pylabhub_lib.datablock_is_process_alive(pid)

def force_reset_slot(shm_name: str, slot_index: int, force: bool = False) -> RecoveryResult:
    return pylabhub_lib.datablock_force_reset_slot(shm_name.encode('utf-8'), slot_index, force)

def force_reset_all_slots(shm_name: str, force: bool = False) -> RecoveryResult:
    return pylabhub_lib.datablock_force_reset_all_slots(shm_name.encode('utf-8'), force)

def release_zombie_readers(shm_name: str, slot_index: int, force: bool = False) -> RecoveryResult:
    return pylabhub_lib.datablock_release_zombie_readers(shm_name.encode('utf-8'), slot_index, force)

def release_zombie_writer(shm_name: str, slot_index: int) -> RecoveryResult:
    return pylabhub_lib.datablock_release_zombie_writer(shm_name.encode('utf-8'), slot_index)

def cleanup_dead_consumers(shm_name: str) -> RecoveryResult:
    return pylabhub_lib.datablock_cleanup_dead_consumers(shm_name.encode('utf-8'))

def validate_integrity(shm_name: str, repair: bool = False) -> RecoveryResult:
    return pylabhub_lib.datablock_validate_integrity(shm_name.encode('utf-8'), repair)

if __name__ == '__main__':
    # This is an example of how to use the recovery API from Python.
    # It assumes that a DataBlock producer (e.g., from an example) is running.
    # To run this, you would typically execute:
    #   python3 src/python/recovery.py
    
    # The shared memory name must match the one used by the producer.
    shm_name = "my_datablock"
    
    print(f"--- Running Recovery & Diagnostics for '{shm_name}' ---")
    
    try:
        # 1. Diagnose all slots
        print("
[1. Diagnosing all slots]")
        all_diags = diagnose_all_slots(shm_name)
        if not all_diags:
            print("  No slots diagnosed. Is a producer running?")
        for d in all_diags:
            print(f"  - {d}")

        # 2. Get detailed diagnostics for a single slot
        if all_diags:
            slot_to_check = all_diags[0].slot_index
            print(f"
[2. Detailed diagnostics for slot {slot_to_check}]")
            diag_one = diagnose_slot(shm_name, slot_to_check)
            print(f"  - Full diagnostics: {diag_one}")
            
            # 3. Check if the writer process is alive
            if diag_one.write_lock != 0:
                print(f"
[3. Checking writer process liveness for PID {diag_one.write_lock}]")
                alive = is_process_alive(diag_one.write_lock)
                print(f"  - Process {diag_one.write_lock} is {'ALIVE' if alive else 'DEAD'}")
                if not alive:
                    print("  - Writer is dead. Recovery might be needed.")
            else:
                print("
[3. No writer process to check (write_lock is 0)]")


        # 4. Clean up dead consumers from the heartbeat table
        print("
[4. Cleaning up dead consumers]")
        cleanup_res = cleanup_dead_consumers(shm_name)
        print(f"  - Cleanup result: {cleanup_res.name}")
        
        # 5. Validate integrity of the DataBlock
        print("
[5. Validating DataBlock integrity]")
        validate_res = validate_integrity(shm_name)
        print(f"  - Validation result: {validate_res.name}")
        if validate_res != RecoveryResult.RECOVERY_SUCCESS:
            print("  - Integrity issues found. Consider running with --repair.")
            
    except RuntimeError as e:
        print(f"
An error occurred: {e}", file=sys.stderr)
        print("Please ensure the DataBlock producer is running and the shared memory name is correct.", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"
An unexpected error occurred: {e}", file=sys.stderr)
        sys.exit(1)

