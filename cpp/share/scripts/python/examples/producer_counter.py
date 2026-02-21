"""
producer_counter.py — Example pylabhub-actor producer script (multi-role API).

Publishes a monotonic counter + timestamp via the role "counter_out".
The slot layout is declared in producer_counter.json — no struct.pack() needed.

Slot fields  (ctypes.LittleEndianStructure, natural alignment):
  count  int64    — monotonic counter
  ts     float64  — Unix timestamp (seconds)

FlexZone fields (persistent writable region, written once in on_init):
  producer_pid  uint64    — PID of this producer process
  start_time    float64   — Unix start time
  label         c_char*32 — channel label string

Usage
-----
    # Run (default mode)
    pylabhub-actor --config producer_counter.json

    # Print slot/flexzone layout and exit
    pylabhub-actor --config producer_counter.json --validate

    # Show configured roles
    pylabhub-actor --config producer_counter.json --list-roles

Notes
-----
- The role name "counter_out" in @actor.on_write() must match the key in the
  "roles" map in producer_counter.json.
- Module-level variables are the natural way to hold state across callbacks.
  There is no ctx.state dict or any C++-managed state dict.
- Do NOT store the `slot` object beyond the on_write() call — it is a
  ctypes.from_buffer view into SHM memory, valid only during that call.
  `flexzone` is persistent and safe to store.
"""

import os
import time

import pylabhub_actor as actor

# ---------------------------------------------------------------------------
# Module-level state (natural Python module variables)
# ---------------------------------------------------------------------------
count = 0


# ---------------------------------------------------------------------------
# Role: counter_out (producer)
# ---------------------------------------------------------------------------

@actor.on_init("counter_out")
def counter_out_init(flexzone, api: actor.ActorRoleAPI):
    """
    Called once after SHM is ready, before the write loop starts.

    flexzone is a ctypes.LittleEndianStructure view backed directly by the
    SHM flexible-zone region.  Writes here go straight to shared memory.

    api.update_flexzone_checksum() recomputes the BLAKE2b checksum so that
    consumers can verify integrity on connect.
    """
    flexzone.producer_pid = os.getpid()
    flexzone.start_time   = time.time()
    flexzone.label        = b"lab.examples.counter"
    api.update_flexzone_checksum()
    api.log('info', f"counter_out: started  uid={api.uid()}  pid={os.getpid()}")


@actor.on_write("counter_out")
def counter_out_write(slot, flexzone, api: actor.ActorRoleAPI) -> bool:
    """
    Called each write-loop iteration (interval_ms=0 -> as fast as SHM allows).

    slot: ctypes.LittleEndianStructure — writable view into the current SHM slot.
    Returns True (or None) to commit; False to discard (no ZMQ broadcast).
    Do NOT store `slot` beyond this call.
    """
    global count
    count += 1
    slot.count = count
    slot.ts    = time.time()

    if count % 1000 == 0:
        api.log('info',
                f"counter_out: slot {count}  "
                f"consumers={len(api.consumers())}")
    return True


@actor.on_message("counter_out")
def counter_out_message(sender: str, data: bytes, api: actor.ActorRoleAPI):
    """Called when a connected consumer sends a ZMQ ctrl frame."""
    api.log('debug', f"counter_out: ctrl from {sender}: {len(data)} bytes")
    api.send(sender, b"ack")


@actor.on_stop("counter_out")
def counter_out_stop(flexzone, api: actor.ActorRoleAPI):
    """Called once after the write loop exits."""
    api.log('info', f"counter_out: stopped after {count} slots")
