# P8: Error Recovery API - Design Specification
**Date:** 2026-02-07  
**Priority:** HIGH (Critical for production operations)  
**Effort:** 2 days design, ~200 lines implementation  
**Dependencies:** P4 (SlotRWCoordinator), P10 (Observability)

---

## PROBLEM STATEMENT

### Production Scenarios Requiring Recovery

**Scenario 1: Stuck Writer**
```
Producer crashes mid-write:
- Slot in WRITING state
- SlotRWState.write_lock held by dead process
- All subsequent writes blocked forever
```

**Scenario 2: Zombie Readers**
```
Consumer crashes without releasing:
- SlotRWState.reader_count > 0
- Writer waits forever for readers to drain
- Slot never available for writing
```

**Scenario 3: Corrupted State**
```
Memory corruption or bug:
- slot_state in invalid value
- reader_count inconsistent
- System behavior undefined
```

**Scenario 4: Stale Heartbeat**
```
Consumer stopped sending heartbeat:
- last_heartbeat_ns > 5 seconds old
- Producer doesn't know if consumer is alive
- Need to clean up consumer slot
```

**Current State:**
- **No recovery tools** → Must restart all processes
- **No diagnostics** → Can't inspect stuck states
- **No cleanup** → Shared memory leaks accumulate
- **No automation** → Manual intervention required

---

## DESIGN GOALS

1. **Safe Recovery:** Minimize risk of data corruption
2. **Observable:** Clear diagnostics before recovery
3. **Automated:** Scriptable for monitoring systems
4. **Manual Override:** CLI tools for emergency use
5. **Auditable:** Log all recovery actions
6. **Minimal Impact:** Don't disturb healthy processes

---

## PROPOSED API

### C API (ABI-Stable, Cross-Language)

```c
// Error recovery API
#ifdef __cplusplus
extern "C" {
#endif

// Diagnostic operations (read-only, safe)
typedef struct {
    uint64_t slot_id;
    uint32_t slot_index;
    uint8_t slot_state;       // FREE, WRITING, COMMITTED, DRAINING
    uint64_t write_lock;      // PID holding write lock (0 if free)
    uint32_t reader_count;
    uint64_t write_generation;
    uint8_t writer_waiting;
    bool is_stuck;            // Heuristic: stuck for >30 seconds
    uint64_t stuck_duration_ms;
} SlotDiagnostic;

// Get diagnostic info for a single slot
int datablock_diagnose_slot(
    const char* shm_name,
    uint32_t slot_index,
    SlotDiagnostic* out
);

// Get diagnostic info for all slots
int datablock_diagnose_all_slots(
    const char* shm_name,
    SlotDiagnostic* out_array,
    size_t array_capacity,
    size_t* out_count
);

// Check if a process is alive
bool datablock_is_process_alive(uint64_t pid);

// Recovery operations (DANGEROUS, use with caution)
typedef enum {
    RECOVERY_SUCCESS = 0,
    RECOVERY_FAILED = 1,
    RECOVERY_UNSAFE = 2,      // Unsafe to recover (active processes)
    RECOVERY_NOT_STUCK = 3,   // Slot not stuck, no action taken
    RECOVERY_INVALID_SLOT = 4
} RecoveryResult;

// Force reset a single slot (DANGEROUS)
RecoveryResult datablock_force_reset_slot(
    const char* shm_name,
    uint32_t slot_index,
    bool force  // true = reset even if unsafe
);

// Force reset all slots (VERY DANGEROUS)
RecoveryResult datablock_force_reset_all_slots(
    const char* shm_name,
    bool force
);

// Release zombie readers (if PID dead)
RecoveryResult datablock_release_zombie_readers(
    const char* shm_name,
    uint32_t slot_index
);

// Release zombie writer (if PID dead)
RecoveryResult datablock_release_zombie_writer(
    const char* shm_name,
    uint32_t slot_index
);

// Clean up dead consumer heartbeat slots
int datablock_cleanup_dead_consumers(
    const char* shm_name,
    uint64_t timeout_ns  // Mark dead if heartbeat older than this
);

// Validate shared memory integrity
typedef struct {
    bool header_valid;
    bool slot_states_valid;
    bool checksum_valid;
    uint32_t corrupted_slots;
    char error_message[256];
} IntegrityCheck;

int datablock_validate_integrity(
    const char* shm_name,
    IntegrityCheck* out
);

#ifdef __cplusplus
}
#endif
```

### C++ Wrapper API (Convenience)

```cpp
namespace pylabhub::recovery {

// Diagnostic functions
class SlotDiagnostics {
public:
    static std::vector<SlotDiagnostic> diagnose_all(
        const std::string& shm_name
    );
    
    static SlotDiagnostic diagnose_slot(
        const std::string& shm_name,
        uint32_t slot_index
    );
    
    static std::vector<SlotDiagnostic> find_stuck_slots(
        const std::string& shm_name,
        std::chrono::milliseconds stuck_threshold = std::chrono::seconds(30)
    );
    
    static bool is_process_alive(uint64_t pid);
};

// Recovery functions (DANGEROUS)
class SlotRecovery {
public:
    // Reset single slot (checks safety first)
    static RecoveryResult reset_slot(
        const std::string& shm_name,
        uint32_t slot_index,
        bool force = false
    );
    
    // Reset all slots (checks safety first)
    static RecoveryResult reset_all_slots(
        const std::string& shm_name,
        bool force = false
    );
    
    // Release zombie readers (only if PID dead)
    static RecoveryResult release_zombie_readers(
        const std::string& shm_name,
        uint32_t slot_index
    );
    
    // Release zombie writer (only if PID dead)
    static RecoveryResult release_zombie_writer(
        const std::string& shm_name,
        uint32_t slot_index
    );
    
    // Auto-recover: diagnose and fix safe issues
    static std::vector<std::string> auto_recover(
        const std::string& shm_name,
        bool dry_run = true  // true = report only, don't fix
    );
};

// Heartbeat cleanup
class HeartbeatManager {
public:
    static std::vector<uint64_t> find_dead_consumers(
        const std::string& shm_name,
        std::chrono::seconds timeout = std::chrono::seconds(5)
    );
    
    static int cleanup_dead_consumers(
        const std::string& shm_name,
        std::chrono::seconds timeout = std::chrono::seconds(5)
    );
};

// Integrity validation
class IntegrityValidator {
public:
    static IntegrityCheck validate(const std::string& shm_name);
    
    static bool is_healthy(const std::string& shm_name);
};

} // namespace pylabhub::recovery
```

---

## SAFETY MECHANISMS

### Rule 1: Never Reset Active Processes

**Check before recovery:**
```cpp
RecoveryResult datablock_force_reset_slot(
    const char* shm_name,
    uint32_t slot_index,
    bool force
) {
    SlotDiagnostic diag;
    datablock_diagnose_slot(shm_name, slot_index, &diag);
    
    // Safety check: Is write lock held by a living process?
    if (diag.write_lock != 0) {
        if (datablock_is_process_alive(diag.write_lock)) {
            if (!force) {
                LOG_ERROR("Slot {} write lock held by ALIVE process {}", 
                          slot_index, diag.write_lock);
                return RECOVERY_UNSAFE;
            } else {
                LOG_WARN("FORCE reset slot {} (overriding safety check)", 
                         slot_index);
            }
        }
    }
    
    // Safety check: Are there living readers?
    if (diag.reader_count > 0 && !force) {
        // TODO: Check if reader PIDs are alive
        // For now, require force=true
        LOG_ERROR("Slot {} has {} readers (may be alive)", 
                  slot_index, diag.reader_count);
        return RECOVERY_UNSAFE;
    }
    
    // Safe to reset
    reset_slot_state(shm_name, slot_index);
    return RECOVERY_SUCCESS;
}
```

### Rule 2: Log All Recovery Actions

```cpp
void reset_slot_state(const char* shm_name, uint32_t slot_index) {
    // Open shared memory
    SlotRWState* rw_state = get_slot_rw_state(shm_name, slot_index);
    
    // Log before
    LOG_WARN("RECOVERY: Resetting slot {} in {}", slot_index, shm_name);
    LOG_WARN("  Before: state={}, write_lock={}, reader_count={}, gen={}",
             rw_state->slot_state.load(),
             rw_state->write_lock.load(),
             rw_state->reader_count.load(),
             rw_state->write_generation.load());
    
    // Reset to FREE state
    rw_state->write_lock.store(0, std::memory_order_release);
    rw_state->reader_count.store(0, std::memory_order_release);
    rw_state->slot_state.store(SLOT_STATE_FREE, std::memory_order_release);
    rw_state->writer_waiting.store(0, std::memory_order_release);
    
    // Log after
    LOG_WARN("  After: state=FREE, write_lock=0, reader_count=0");
    LOG_WARN("RECOVERY: Slot {} reset complete", slot_index);
    
    // Record in metrics
    header->recovery_actions.fetch_add(1, std::memory_order_relaxed);
    header->last_recovery_timestamp_ns.store(
        get_timestamp_ns(), 
        std::memory_order_relaxed
    );
}
```

### Rule 3: Dry-Run Mode for Testing

```cpp
std::vector<std::string> auto_recover(
    const std::string& shm_name,
    bool dry_run
) {
    std::vector<std::string> actions;
    
    // Diagnose all slots
    auto stuck_slots = SlotDiagnostics::find_stuck_slots(shm_name);
    
    for (const auto& slot : stuck_slots) {
        // Check if write lock is held by dead process
        if (slot.write_lock != 0 && 
            !SlotDiagnostics::is_process_alive(slot.write_lock)) {
            
            if (dry_run) {
                actions.push_back(fmt::format(
                    "Would release zombie writer on slot {} (PID {})",
                    slot.slot_index, slot.write_lock
                ));
            } else {
                SlotRecovery::release_zombie_writer(shm_name, slot.slot_index);
                actions.push_back(fmt::format(
                    "Released zombie writer on slot {} (PID {})",
                    slot.slot_index, slot.write_lock
                ));
            }
        }
        
        // Check for zombie readers (implementation TBD)
        // ...
    }
    
    return actions;
}
```

---

## CLI TOOLS SPECIFICATION

### `datablock-admin` Command-Line Tool

**Usage:**
```bash
datablock-admin <command> [options]
```

**Commands:**

#### 1. `diagnose` - Inspect slot states

```bash
# Diagnose all slots
$ datablock-admin diagnose --shm-name datablock_sensor_12345

DataBlock: datablock_sensor_12345
Status: 3 slots, 1 stuck, 2 healthy

Slot 0: FREE                    ✓ healthy
Slot 1: WRITING (stuck 45s)     ⚠️  STUCK
  - Write lock: PID 12345 (DEAD)
  - Duration: 45 seconds
  - Recommendation: Run 'force-reset-slot 1'

Slot 2: COMMITTED               ✓ healthy
  - Readers: 2
  - Generation: 1234

# Diagnose single slot
$ datablock-admin diagnose --shm-name foo --slot 1

Slot 1 Diagnostic:
  State: WRITING
  Write Lock: PID 12345 (DEAD)
  Reader Count: 0
  Write Generation: 567
  Writer Waiting: 0
  Stuck: YES (45 seconds)
  
Recommendation: This slot is safe to reset (writer is dead)
Run: datablock-admin force-reset-slot --shm-name foo --slot 1
```

#### 2. `force-reset-slot` - Reset a single stuck slot

```bash
# Reset with safety check
$ datablock-admin force-reset-slot --shm-name foo --slot 1

[WARN] Slot 1 is stuck (write lock PID 12345)
[INFO] Checking if PID 12345 is alive... DEAD
[INFO] Safe to reset
[WARN] Resetting slot 1...
[OK] Slot 1 reset to FREE

# Force reset (override safety)
$ datablock-admin force-reset-slot --shm-name foo --slot 1 --force

[WARN] FORCE mode enabled (ignoring safety checks)
[WARN] Resetting slot 1...
[OK] Slot 1 reset to FREE
```

#### 3. `auto-recover` - Automatic recovery

```bash
# Dry run (report only)
$ datablock-admin auto-recover --shm-name foo --dry-run

Found 2 issues:
  [1] Slot 1: Zombie writer (PID 12345, DEAD)
      Action: Release write lock
  [2] Consumer heartbeat: PID 67890 timeout (last seen 10s ago)
      Action: Clear heartbeat slot

Run without --dry-run to apply fixes.

# Apply fixes
$ datablock-admin auto-recover --shm-name foo

Applying fixes...
  [1] Released zombie writer on slot 1 (PID 12345)
  [2] Cleared dead consumer heartbeat (PID 67890)

Recovery complete: 2 actions applied
```

#### 4. `validate` - Check integrity

```bash
$ datablock-admin validate --shm-name foo

Validating datablock_sensor_12345...

Header:             ✓ Valid
Slot States:        ✓ Valid
Checksums:          ✓ Valid
Corrupted Slots:    0

Overall: ✓ HEALTHY
```

#### 5. `cleanup-heartbeats` - Remove dead consumer slots

```bash
$ datablock-admin cleanup-heartbeats --shm-name foo --timeout 5s

Checking consumer heartbeats (timeout: 5s)...

Consumer 1: PID 11111, last seen 2s ago    ✓ alive
Consumer 2: PID 22222, last seen 12s ago   ✗ DEAD (cleaned up)
Consumer 3: PID 33333, last seen 1s ago    ✓ alive

Cleanup complete: 1 dead consumer removed
```

---

## PYTHON BINDINGS

```python
from pylabhub_admin import (
    diagnose_slot,
    diagnose_all_slots,
    force_reset_slot,
    auto_recover,
    cleanup_dead_consumers,
    validate_integrity
)

# Diagnose
diagnostics = diagnose_all_slots("datablock_sensor_12345")
for diag in diagnostics:
    if diag.is_stuck:
        print(f"Slot {diag.slot_index} is stuck for {diag.stuck_duration_ms}ms")
        
        # Check if safe to recover
        if diag.write_lock != 0:
            from pylabhub_admin import is_process_alive
            if not is_process_alive(diag.write_lock):
                print(f"  Writer PID {diag.write_lock} is DEAD, safe to reset")

# Auto-recover (dry run)
actions = auto_recover("datablock_sensor_12345", dry_run=True)
for action in actions:
    print(f"Would perform: {action}")

# Auto-recover (apply)
actions = auto_recover("datablock_sensor_12345", dry_run=False)
for action in actions:
    print(f"Performed: {action}")

# Validate integrity
integrity = validate_integrity("datablock_sensor_12345")
if not integrity.header_valid:
    print("ERROR: Header corrupted!")
if integrity.corrupted_slots > 0:
    print(f"WARNING: {integrity.corrupted_slots} slots corrupted")
```

---

## MONITORING INTEGRATION

### Prometheus Metrics Export

```python
# Monitor for stuck slots
from pylabhub_admin import diagnose_all_slots
from prometheus_client import Gauge

stuck_slots_gauge = Gauge('datablock_stuck_slots', 'Number of stuck slots', ['shm_name'])

def monitor_datablock(shm_name):
    diagnostics = diagnose_all_slots(shm_name)
    stuck_count = sum(1 for d in diagnostics if d.is_stuck)
    stuck_slots_gauge.labels(shm_name=shm_name).set(stuck_count)
    
    if stuck_count > 0:
        # Alert
        alert("DataBlock stuck slots detected", shm_name, stuck_count)
```

### Auto-Recovery Daemon

```python
import time
from pylabhub_admin import auto_recover

def recovery_daemon(shm_names, check_interval=60):
    """
    Periodically check and auto-recover stuck slots.
    """
    while True:
        for shm_name in shm_names:
            try:
                actions = auto_recover(shm_name, dry_run=False)
                if actions:
                    log.warning(f"Auto-recovery on {shm_name}: {actions}")
            except Exception as e:
                log.error(f"Recovery failed on {shm_name}: {e}")
        
        time.sleep(check_interval)

# Run in background
import threading
recovery_thread = threading.Thread(
    target=recovery_daemon,
    args=(["datablock_sensor_12345", "datablock_camera_67890"], 60),
    daemon=True
)
recovery_thread.start()
```

---

## EMERGENCY PROCEDURES DOCUMENTATION

### Appendix H: Emergency Recovery Procedures

#### Procedure 1: Stuck Writer Slot

**Symptoms:**
- Producer `acquire_write_slot()` times out
- Metrics show high `writer_timeout_count`
- Slot stuck in WRITING state for >30 seconds

**Diagnosis:**
```bash
$ datablock-admin diagnose --shm-name <name>
```

**Recovery:**
1. Check if writer process is alive
2. If dead, run `force-reset-slot --slot <N>`
3. If alive, investigate why writer is stuck (deadlock?)

**Prevention:**
- Use Layer 2 Transaction API (auto-release on exception)
- Monitor `writer_timeout_count` metric

---

#### Procedure 2: Zombie Readers

**Symptoms:**
- Writer waits forever for readers to drain
- Metrics show `reader_peak_count` > expected
- Readers that never decrement

**Diagnosis:**
```bash
$ datablock-admin diagnose --shm-name <name> --slot <N>
```

**Recovery:**
1. Identify which processes are readers (need PID tracking)
2. Check if reader processes are alive
3. If dead, run `release-zombie-readers --slot <N>`

**Prevention:**
- Use Layer 2 Transaction API (auto-release)
- Implement PID tracking for readers (future work)

---

#### Procedure 3: Corrupted Header

**Symptoms:**
- Segfaults on attach
- Invalid slot states
- Integrity check fails

**Diagnosis:**
```bash
$ datablock-admin validate --shm-name <name>
```

**Recovery:**
1. **DESTRUCTIVE:** Delete shared memory segment
2. Restart producer (will recreate)
3. Restart consumers

**Prevention:**
- Use checksums
- Monitor integrity regularly

---

## IMPLEMENTATION CHECKLIST

- [ ] Implement C API functions (diagnostic and recovery)
- [ ] Implement PID liveness check (platform-specific)
- [ ] Implement C++ wrapper classes
- [ ] Implement `datablock-admin` CLI tool
- [ ] Implement Python bindings (ctypes)
- [ ] Add unit tests (mock stuck states)
- [ ] Add integration tests (real stuck scenarios)
- [ ] Write emergency procedures documentation
- [ ] Add monitoring examples (Prometheus)
- [ ] Add auto-recovery daemon example

---

## ESTIMATED EFFORT

| Task | Lines of Code | Effort |
|------|---------------|--------|
| C API implementation | ~300 | 1 day |
| C++ wrapper classes | ~150 | 0.5 days |
| CLI tool | ~200 | 0.5 days |
| Python bindings | ~100 | 0.5 days |
| Tests | ~200 | 0.5 days |
| Documentation | ~300 lines | 0.5 days |
| **Total** | **~1,250** | **3.5 days** |

---

## SAFETY CONSIDERATIONS

### What Can Go Wrong

1. **Reset active slot** → Data corruption, crashes
2. **Race with producer** → Slot reset during write
3. **False positive PID check** → PID reused by new process

### Mitigation

1. **Dry-run by default** → User must confirm
2. **Safety checks** → Never reset if process alive
3. **Logging** → Audit trail of all actions
4. **Metrics** → Track recovery actions
5. **Documentation** → Clear warnings about risks

---

## SUMMARY

**P8: Error Recovery API provides:**
- ✅ Diagnostic tools (inspect stuck states)
- ✅ Safe recovery (check PID liveness)
- ✅ CLI tools (emergency operations)
- ✅ Python bindings (scripting/monitoring)
- ✅ Auto-recovery (automated cleanup)
- ✅ Logging and auditing (track all actions)

**Recommendation:** **APPROVE AND IMPLEMENT**

This completes P8. Ready to move on to P9 (Schema Validation)?
