"""
consumer_logger.py — Example pylabhub-actor consumer script (schema API).

Subscribes to the counter channel from producer_counter and logs each slot.
The slot layout is declared in consumer_logger.json — no struct.unpack() needed.

Slot fields (ctypes.LittleEndianStructure):
  count  int64    — monotonic counter from producer
  ts     float64  — producer Unix timestamp

FlexZone fields (read-only view):
  producer_pid  uint64   — producer PID
  start_time    float64  — producer start time
  label         c_char*32 — channel label

Module-level variables replace the old ctx.state dict.

Usage
-----
    pylabhub-actor --config consumer_logger.json
    (Requires producer_counter to be running on the same broker.)
"""

import time

# Module-level state.
last_count  = 0
slots_read  = 0
start_time  = 0.0


def on_init(flexzone, api):
    """Called once before subscriptions are active. flexzone is readable."""
    global start_time
    start_time = time.time()
    pid    = flexzone.producer_pid
    label  = flexzone.label.decode(errors='replace') if flexzone.label else ''
    fz_ok  = api.verify_flexzone_checksum()
    api.log('info',
            f"consumer_logger: connected  producer_pid={pid}  "
            f"label='{label}'  flexzone_valid={fz_ok}")


def on_read(slot, flexzone, api):
    """
    Called for each SHM slot.

    slot:    ctypes.LittleEndianStructure (SlotFrame) — read-only copy.
    flexzone: ctypes struct — local copy of flexzone (writable locally;
              changes do NOT propagate to SHM).
    api.slot_valid(): False when slot checksum failed and on_checksum_fail='pass'.

    Lifetime of slot: valid only during this call.
    """
    global last_count, slots_read

    if not api.slot_valid():
        api.log('warn', f"slot checksum failed — accepting data as-is")

    count = slot.count
    ts    = slot.ts
    slots_read += 1

    skipped = count - last_count - 1
    last_count = count

    if skipped > 0:
        api.log('warn', f"skipped {skipped} slot(s) at count={count}")

    if count % 1000 == 0:
        elapsed = time.time() - start_time
        rate = slots_read / elapsed if elapsed > 0 else 0.0
        api.log('info',
                f"slot {count}  ts={ts:.3f}  rate={rate:.0f} slots/s")


def on_data(data, api):
    """
    Called for each ZMQ broadcast frame from the producer.
    In SHM mode this fires for non-slot ZMQ messages.
    In ZMQ-only mode (shm.enabled=false) this is the primary data path.
    """
    api.log('debug', f"zmq frame: {len(data)} bytes")


def on_stop(flexzone, api):
    elapsed = time.time() - start_time
    api.log('info',
            f"consumer_logger: stopped. "
            f"read {slots_read} slots in {elapsed:.1f}s")
