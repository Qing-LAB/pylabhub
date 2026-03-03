"""
Producer: DemoProducer

Publishes a monotonic counter, Unix timestamp, and sine-wave value
every 100 ms (10 Hz) on channel 'lab.demo.counter'.

Slot layout (from producer.json slot_schema):
  count  int64    — monotonic counter starting at 1
  ts     float64  — Unix timestamp (seconds since epoch)
  value  float32  — sine wave: sin(count * 0.1) * 10.0

FlexZone layout (from producer.json flexzone_schema):
  producer_pid  uint64    — PID of this process
  start_ts      float64   — timestamp when on_init ran
  label         c_char*32 — channel label

Object lifetime rules
---------------------
  out_slot — Valid ONLY during on_produce(). Do not store.
  flexzone — Valid for the producer's entire lifetime. Safe to store.
  api      — Always valid. Stateless proxy to C++ services.

To replace this logic: edit only this file. The C++ runtime and the
JSON config are unchanged — a different script drops straight in.
"""

import math
import os
import time

import pylabhub_producer as prod

# ---------------------------------------------------------------------------
# Module-level state  (GIL serialises callbacks — no locking needed)
# ---------------------------------------------------------------------------
_count: int   = 0
_start: float = 0.0


def on_init(api: prod.ProducerAPI) -> None:
    """
    Called once after the producer SHM region is ready.

    Initialise the flexzone with metadata and stamp its BLAKE2b checksum so
    that consumers can verify integrity when they first connect.
    """
    global _start
    _start = time.time()

    fz = api.flexzone()
    if fz is not None:
        fz.producer_pid = os.getpid()
        fz.start_ts     = _start
        fz.label        = b"lab.demo.counter"
        api.update_flexzone_checksum()

    api.log('info',
            f"DemoProducer: started  uid={api.uid()}  pid={os.getpid()}")


def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:
    """
    Called every 100 ms by the C++ timer loop.

    out_slot is a writable ctypes.LittleEndianStructure backed by the next SHM
    slot.  Fill fields and return True to commit (publish to consumers).
    Return False to discard (slot is not published, no ZMQ broadcast).

    When no SHM slot is available within the timeout, out_slot is None.
    In that case return False (or None) to skip this cycle.
    """
    global _count

    if out_slot is None:
        return False  # SHM slot unavailable — backpressure

    _count += 1
    out_slot.count = _count
    out_slot.ts    = time.time()
    out_slot.value = math.sin(_count * 0.1) * 10.0

    for sender, data in messages:
        api.log('debug', f"DemoProducer: ctrl from {sender}: {data!r}")
        api.send(sender, b"ack")

    if _count % 100 == 0:  # log every 10 s at 10 Hz
        api.log('info',
                f"DemoProducer: count={_count}  value={out_slot.value:.3f}"
                f"  consumers={len(api.consumers())}")
    return True


def on_stop(api: prod.ProducerAPI) -> None:
    """Called once after the write loop exits (on shutdown or api.stop())."""
    elapsed = time.time() - _start
    api.log('info',
            f"DemoProducer: stopped  total={_count}  elapsed={elapsed:.1f}s")
