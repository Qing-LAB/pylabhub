"""
producer_counter.py — Example pylabhub-actor producer script (schema API).

Publishes a monotonic counter + timestamp. The slot layout is declared once
in producer_counter.json — no struct.pack() needed in the script.

Slot fields (ctypes.LittleEndianStructure):
  count  int64    — monotonic counter
  ts     float64  — Unix timestamp (seconds)

FlexZone fields:
  producer_pid  uint64  — PID of the producer process
  start_time    float64 — Unix start time
  label         c_char*32 — channel label string

Module-level variables replace the old ctx.state dict.

Usage
-----
    pylabhub-actor --config producer_counter.json
    pylabhub-actor --config producer_counter.json --validate
"""

import os
import time

# Module-level state (replaces ctx.state).
count = 0


def on_init(flexzone, api):
    """Called once before the write loop. flexzone is writable for producer lifetime."""
    import ctypes
    flexzone.producer_pid = os.getpid()
    flexzone.start_time   = time.time()
    flexzone.label        = b"lab.examples.counter"
    api.update_flexzone_checksum()   # stamp the SHM flexzone checksum
    api.log('info', f"producer_counter: started on channel, pid={os.getpid()}")


def on_write(slot, flexzone, api) -> bool:
    """
    Called each write-loop iteration.

    slot: ctypes.LittleEndianStructure (SlotFrame) — writable view into SHM.
    Returns True (or None) to commit. Returns False to discard this slot.

    Lifetime: slot is ONLY valid during this call. Do not store it.
    """
    global count
    count += 1
    slot.count = count
    slot.ts    = time.time()

    if count % 1000 == 0:
        api.log('info', f"producer_counter: slot {count}  "
                f"consumers={len(api.consumers())}")

    return True


def on_message(sender, data, api):
    """Called when a consumer sends a ZMQ ctrl message."""
    api.log('debug', f"ctrl from {sender}: {len(data)} bytes")
    api.send(sender, b"ack")


def on_stop(flexzone, api):
    """Called once after the write loop stops."""
    api.log('info', f"producer_counter: stopped after {count} slots")
