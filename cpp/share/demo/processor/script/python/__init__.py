"""
Processor: DemoDoubler

Reads each slot from 'lab.demo.counter', applies a simple transformation
(doubles the value), and publishes the result on 'lab.demo.processed'.

This is the middle stage in the demo pipeline:
  DemoProducer → lab.demo.counter → (here) → lab.demo.processed → DemoConsumer

Input slot  (in_slot_schema in processor.json):
  count  int64    — monotonic counter from the producer
  ts     float64  — Unix timestamp from the producer
  value  float32  — raw sine-wave value from the producer

Output slot (out_slot_schema in processor.json):
  count   int64    — copied from input unchanged
  ts      float64  — copied from input unchanged
  value   float32  — copied from input unchanged
  doubled float32  — value * 2.0 (the transformation applied here)

Object lifetime rules
---------------------
  in_slot  — Read-only ctypes view. Valid ONLY during on_process(). Do not store.
  out_slot — Writable ctypes view. Valid ONLY during on_process(). Do not store.
  api      — Always valid. Stateless proxy to C++ services.

To replace this logic: edit only this file.  The C++ runtime and the
processor.json config are unchanged — a different script drops straight in.
"""

import pylabhub_processor as proc

# ---------------------------------------------------------------------------
# Module-level state
# ---------------------------------------------------------------------------
_processed: int = 0
_dropped:   int = 0


def on_init(api: proc.ProcessorAPI) -> None:
    """Called once before the processing loop starts."""
    api.log('info',
            f"DemoDoubler: started  uid={api.uid()}"
            f"  {api.in_channel()} → {api.out_channel()}")


def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
    """
    Called for each input slot acquired from 'lab.demo.counter'.

    in_slot  — read-only ctypes struct (do not write, do not store)
    out_slot — writable ctypes struct (write, then return True to commit)
    flexzone — output-side persistent flexzone (None: not configured)
    messages — list of (sender: str, data: bytes) from ZMQ peer channel
    api      — ProcessorAPI: log, broadcast, stop, counters, etc.

    Return True or None to commit out_slot (publish on lab.demo.processed).
    Return False to discard (output slot not published, increments drop count).
    When in_slot is None (timeout), return value is ignored.
    """
    global _processed, _dropped

    if in_slot is None:
        # Timeout — no input slot in timeout_ms.
        api.log('debug', "DemoDoubler: timeout — no slot from producer in 5 s")
        return False

    for sender, data in messages:
        api.log('debug', f"DemoDoubler: zmq from {sender}: {data!r}")

    # ── Transform ─────────────────────────────────────────────────────────────
    # Copy unchanged fields.
    out_slot.count = in_slot.count
    out_slot.ts    = in_slot.ts
    out_slot.value = in_slot.value
    # Apply transformation.
    out_slot.doubled = in_slot.value * 2.0

    _processed += 1
    if _processed % 100 == 0:  # log every 10 s at 10 Hz
        api.log('info',
                f"DemoDoubler: processed={_processed}"
                f"  in={api.in_slots_received()}"
                f"  out={api.out_slots_written()}"
                f"  drops={api.out_drop_count()}")

    return True  # commit: publish out_slot on lab.demo.processed


def on_stop(api: proc.ProcessorAPI) -> None:
    """Called once after the processing loop exits."""
    api.log('info',
            f"DemoDoubler: stopped  processed={_processed}"
            f"  in={api.in_slots_received()}"
            f"  out={api.out_slots_written()}"
            f"  drops={api.out_drop_count()}")
