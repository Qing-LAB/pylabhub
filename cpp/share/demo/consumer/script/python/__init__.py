"""
Consumer: DemoConsumer

Reads each slot from 'lab.demo.processed' and logs the values.

This is the final stage in the demo pipeline:
  DemoProducer → lab.demo.counter → DemoDoubler → lab.demo.processed → (here)

Input slot (slot_schema in consumer.json):
  count   int64    — monotonic counter from the producer
  ts      float64  — Unix timestamp from the producer
  value   float32  — raw sine-wave value from the producer
  doubled float32  — value * 2.0 (applied by the processor)

Object lifetime rules
---------------------
  in_slot  — Read-only ctypes copy. Valid ONLY during on_consume(). Do not store.
  flexzone — None (not configured for this demo consumer).
  api      — Always valid. Stateless proxy to C++ services.

To replace this logic: edit only this file.  The C++ runtime and the
consumer.json config are unchanged — a different script drops straight in.
"""

import time

import pylabhub_consumer as cons

# ---------------------------------------------------------------------------
# Module-level state
# ---------------------------------------------------------------------------
_received: int   = 0
_last_ts:  float = 0.0


def on_init(api: cons.ConsumerAPI) -> None:
    """Called once before the consumption loop starts."""
    api.log('info',
            f"DemoConsumer: started  uid={api.uid()}")


def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:
    """
    Called for each slot received from 'lab.demo.processed'.

    in_slot  — read-only ctypes struct copy (None on timeout)
    flexzone — None (not configured)
    messages — list of bytes from ZMQ data channel
    api      — ConsumerAPI: log, stop, etc.
    """
    global _received, _last_ts

    if in_slot is None:
        api.log('debug', "DemoConsumer: timeout — no slot from processor in 5 s")
        return

    _received += 1
    now = time.time()
    rate = 1.0 / (now - _last_ts) if _last_ts > 0 else 0.0
    _last_ts = now

    if _received % 50 == 0:  # log every 5 s at 10 Hz
        api.log('info',
                f"DemoConsumer: count={in_slot.count}"
                f"  value={in_slot.value:.3f}"
                f"  doubled={in_slot.doubled:.3f}"
                f"  rate={rate:.1f} Hz"
                f"  total={_received}")
    else:
        # Print to stdout so the terminal shows live data
        print(f"  count={in_slot.count:6d}  ts={in_slot.ts:.3f}"
              f"  value={in_slot.value:8.3f}  doubled={in_slot.doubled:8.3f}"
              f"  rate={rate:5.1f} Hz",
              flush=True)


def on_stop(api: cons.ConsumerAPI) -> None:
    """Called once after the consumption loop exits."""
    api.log('info',
            f"DemoConsumer: stopped  total_received={_received}")
