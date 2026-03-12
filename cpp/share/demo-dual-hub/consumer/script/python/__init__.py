"""
Dual-hub demo consumer — reads 'lab.bridge.processed' from Hub B.
Stops after MAX_SLOTS slots, cascading channel_closing to all upstream roles.

Slot: count(int64), ts(float64), value(float32), doubled(float32)
Prints a one-line summary per second and validates doubled == value * 2.
"""

import time

import pylabhub_consumer as cons

MAX_SLOTS = 1000

_received: int    = 0
_window_slots: int = 0
_window_start: float = 0.0
_last_count: int  = 0
_last_value: float = 0.0
_last_doubled: float = 0.0
_prev_count: int  = 0
_overflow_events: int = 0
_bad_doubled: int = 0


def on_init(api: cons.ConsumerAPI) -> None:
    global _window_start
    _window_start = time.time()
    api.log('info', f"BridgeConsumer: started  uid={api.uid()}")


def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:
    global _received, _window_slots, _window_start
    global _last_count, _last_value, _last_doubled
    global _prev_count, _overflow_events, _bad_doubled

    if in_slot is None:
        return

    _received += 1
    if _received >= MAX_SLOTS:
        api.log('info', f"BridgeConsumer: reached {MAX_SLOTS} slots — done")
        api.stop()
        return
    _window_slots += 1
    _last_count   = int(in_slot.count)
    _last_value   = float(in_slot.value)
    _last_doubled = float(in_slot.doubled)

    # Correctness: doubled must equal value * 2
    if abs(_last_doubled - _last_value * 2.0) > 1e-4:
        _bad_doubled += 1

    # Overflow detection via count gaps
    if _received > 1:
        gap = _last_count - _prev_count - 1
        if gap > 0:
            _overflow_events += 1
    _prev_count = _last_count

    now = time.time()
    dt = now - _window_start
    if dt >= 1.0:
        slots_per_sec = _window_slots / dt
        correct = "OK" if _bad_doubled == 0 else f"MISMATCH({_bad_doubled})"
        print(
            f"slots/s={slots_per_sec:<7.1f}  "
            f"count={_last_count:<8d}  "
            f"value={_last_value:<8.3f}  doubled={_last_doubled:<8.3f}  "
            f"overflow={_overflow_events:<4d}  "
            f"doubled_check={correct}",
            flush=True,
        )
        _window_slots = 0
        _window_start = now


def on_stop(api: cons.ConsumerAPI) -> None:
    api.log(
        'info',
        f"BridgeConsumer: stopped  total_received={_received} "
        f"overflow_events={_overflow_events} bad_doubled={_bad_doubled}",
    )
