# Emergency Recovery Procedures for DataBlocks

This document provides guidance on how to diagnose and recover from common failure
scenarios in shared memory `DataBlock`s using the provided recovery tools.

## Tools Overview

Two primary tools are available for recovery:

1.  **`datablock-admin` CLI Tool**: A command-line utility for quick diagnostics
    and recovery actions.
2.  **Python Recovery API**: A Python module (`pylabhub.recovery`) that provides
    programmatic access to the recovery functions.

## Common Scenarios and Recovery Steps

### Scenario 1: Stuck Writer

A "stuck writer" occurs when a producer process acquires a write lock on a slot
and then crashes before releasing it. This will prevent any new data from being
written to that slot.

**Diagnosis:**

Use the `diagnose` command of the `datablock-admin` tool:

```bash
./build/bin/datablock-admin diagnose my_datablock --slot 0
```

Look for a slot where `is_stuck` is `Yes` and `write_lock` has a non-zero PID.
Use `is_process_alive` in the Python API or a system utility to confirm the
process is dead.

**Recovery:**

Use the `recover` command with the `release_writer` action:

```bash
./build/bin/datablock-admin recover my_datablock --slot 0 --action release_writer
```

This will release the lock and reset the slot to a `FREE` state.

### Scenario 2: Stuck Readers

A "stuck reader" can occur if a consumer crashes while holding a read lock. This
is less severe than a stuck writer but can prevent the slot from being reused.

**Diagnosis:**

Use the `diagnose` command. A non-zero `reader_count` on a slot that hasn't
been updated in a long time can indicate stuck readers, especially if the
writer is also blocked (`writer_waiting` is 1).

**Recovery:**

Use the `recover` command with the `release_readers` action. The `--force` flag
may be necessary if the producer is still alive.

```bash
./build/bin/datablock-admin recover my_datablock --slot 0 --action release_readers --force
```

### Scenario 3: Dead Consumers

If a consumer process crashes, its heartbeat will stop, but it will still occupy
a slot in the consumer table, and `active_consumer_count` will be incorrect.

**Diagnosis:**

This is diagnosed by the `cleanup` command itself.

**Recovery:**

Run the `cleanup` command to scan the heartbeat table and remove dead consumers:

```bash
./build/bin/datablock-admin cleanup my_datablock
```

### Scenario 4: Data Corruption

Data corruption can occur due to bugs or hardware issues. The `validate`
command checks the integrity of the DataBlock's metadata and checksums.

**Diagnosis:**

Run the `validate` command:

```bash
./build/bin/datablock-admin validate my_datablock
```

**Recovery:**

If corruption is detected, you can attempt to repair it by recalculating
checksums. **This is a potentially dangerous operation.**

```bash
./build/bin/datablock-admin validate my_datablock --repair
```
