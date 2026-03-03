"""
consumer_logger.py — Example pylabhub-consumer script.

Subscribes to the counter channel and logs each slot.
The slot layout is declared in consumer_logger.json — no struct.unpack() needed.

Slot fields  (ctypes.LittleEndianStructure, natural alignment):
  count  int64    — monotonic counter from producer
  ts     float64  — producer Unix timestamp

FlexZone fields (read-only copy of producer's SHM region):
  producer_pid  uint64    — producer PID
  start_time    float64   — producer start time
  label         c_char*32 — channel label

Usage
-----
    # Run (place this file + consumer_logger.json in a consumer directory)
    pylabhub-consumer <cons_dir>

    # Or point directly at the config
    pylabhub-consumer --config consumer_logger.json

Notes
-----
- `in_slot` is a ctypes copy of the SHM slot — valid only during on_consume().
  Writing to slot fields raises AttributeError. Do not store `in_slot`.
- `flexzone` is a read-only ctypes copy of the producer's flexzone.
- `in_slot=None` means the slot-acquire timed out (timeout_ms elapsed).
"""

import time

import pylabhub_consumer as cons

# ---------------------------------------------------------------------------
# Module-level state
# ---------------------------------------------------------------------------
last_count: int   = 0
slots_read: int   = 0
start_time: float = 0.0


# ---------------------------------------------------------------------------
# Callbacks
# ---------------------------------------------------------------------------

def on_init(api: cons.ConsumerAPI) -> None:
    """Called once before the read loop starts."""
    global start_time
    start_time = time.time()
    api.log('info', f"CounterLogger: started  uid={api.uid()}")


def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:
    """
    Called for each SHM slot (or on timeout when in_slot is None).

    in_slot:  read-only ctypes struct copy (None on timeout)
    flexzone: read-only flexzone copy (may be None if not configured)
    messages: list of bytes from ZMQ data channel
    api:      ConsumerAPI — log, stop, in_slots_received(), etc.
    """
    global last_count, slots_read

    if in_slot is None:
        api.log('debug', "CounterLogger: timeout — no slot in 5 s")
        return

    slots_read += 1
    count = in_slot.count
    ts    = in_slot.ts

    skipped = count - last_count - 1
    last_count = count

    if skipped > 0:
        api.log('warn', f"CounterLogger: skipped {skipped} slot(s) at count={count}")

    elapsed = time.time() - start_time
    rate    = slots_read / elapsed if elapsed > 0 else 0.0

    print(f"[CounterLogger] count={count}  ts={ts:.3f}  "
          f"rate={rate:.0f}/s  skipped={skipped}", flush=True)

    if count % 1000 == 0:
        api.log('info',
                f"CounterLogger: slot {count}  ts={ts:.3f}  "
                f"rate={rate:.0f} slots/s")


def on_stop(api: cons.ConsumerAPI) -> None:
    """Called once after the read loop exits."""
    elapsed = time.time() - start_time
    api.log('info',
            f"CounterLogger: stopped  read={slots_read} slots"
            f"  elapsed={elapsed:.1f}s")
