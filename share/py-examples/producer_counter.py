"""
producer_counter.py — Example pylabhub-producer script.

Publishes a monotonic counter + timestamp via 'lab.examples.counter'.
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
    # Run (place this file + producer_counter.json in a producer directory)
    pylabhub-producer <prod_dir>

    # Or point directly at the config
    pylabhub-producer --config producer_counter.json

    # Validate config and exit
    pylabhub-producer --config producer_counter.json --validate

Notes
-----
- Module-level variables are the natural way to hold state across callbacks.
- Do NOT store the `out_slot` object beyond the on_produce() call — it is a
  ctypes.from_buffer view into SHM memory, valid only during that call.
- `flexzone` is persistent and safe to store between calls.
"""

import os
import time

import pylabhub_producer as prod

# ---------------------------------------------------------------------------
# Module-level state
# ---------------------------------------------------------------------------
count = 0
_fz   = None   # persistent reference to flexzone


# ---------------------------------------------------------------------------
# Callbacks
# ---------------------------------------------------------------------------

def on_init(api: prod.ProducerAPI) -> None:
    """Called once after SHM is ready, before the write loop starts."""
    global _fz
    _fz = api.flexzone()
    if _fz is not None:
        _fz.producer_pid = os.getpid()
        _fz.start_time   = time.time()
        _fz.label        = b"lab.examples.counter"
        api.update_flexzone_checksum()
    api.log('info', f"CounterProducer: started  uid={api.uid()}  pid={os.getpid()}")


def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:
    """
    Called each write-loop iteration (interval_ms=0 → as fast as SHM allows).

    out_slot: ctypes.LittleEndianStructure — writable view into the current SHM slot.
    Returns True (or None) to commit; False to discard (no ZMQ broadcast).
    Do NOT store `out_slot` beyond this call.
    """
    global count
    if out_slot is None:
        return False  # SHM slot unavailable — backpressure

    count += 1
    out_slot.count = count
    out_slot.ts    = time.time()

    for sender, data in messages:
        api.log('debug', f"CounterProducer: ctrl from {sender}: {len(data)} bytes")
        api.send(sender, b"ack")

    if count % 1000 == 0:
        api.log('info',
                f"CounterProducer: slot {count}"
                f"  consumers={len(api.consumers())}")
    return True


def on_stop(api: prod.ProducerAPI) -> None:
    """Called once after the write loop exits."""
    api.log('info', f"CounterProducer: stopped after {count} slots")
