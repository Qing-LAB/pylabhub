"""Throughput consumer: reports slots/s, MB/s, and correctness check every second.
Stops after MAX_SLOTS slots, which cascades channel_closing to all upstream roles."""

import ctypes
import time

import pylabhub_consumer as cons

try:
    import numpy as np
except Exception:
    np = None

BLOCK_SIZE = 1024
EXPECTED_SCALE = 2.0
MAX_SLOTS = 1000

_received: int = 0
_window_slots: int = 0
_window_bytes: int = 0
_window_start: float = 0.0
_slot_bytes: int = 0
_last_count: int = 0
_sample0: float = 0.0
_sample_last: float = 0.0
_scale: float = 0.0
_prev_count: int = 0
_overflow_events: int = 0
_overflow_slots: int = 0
_window_overflow_events: int = 0
_window_overflow_slots: int = 0
_bad_scale: int = 0


def on_init(api: cons.ConsumerAPI) -> None:
    global _window_start
    _window_start = time.time()
    api.log("info", f"DemoConsumer: started uid={api.uid()} throughput window=1s")


def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:
    global _received, _window_slots, _window_bytes, _window_start, _slot_bytes
    global _last_count, _sample0, _sample_last, _scale
    global _prev_count, _overflow_events, _overflow_slots
    global _window_overflow_events, _window_overflow_slots, _bad_scale

    # Propagate shutdown: when the processor stops, the channel closes here too.
    for msg in messages:
        if isinstance(msg, dict) and msg.get("event") == "channel_closing":
            api.log("info", "DemoConsumer: channel closed — stopping")
            api.stop()
            return

    if in_slot is None:
        return

    if _slot_bytes == 0:
        _slot_bytes = ctypes.sizeof(type(in_slot))
        api.log(
            "info",
            f"DemoConsumer: slot_bytes={_slot_bytes} payload={BLOCK_SIZE}xfloat32 + scale:float32",
        )

    _received += 1
    if _received >= MAX_SLOTS:
        api.log("info", f"DemoConsumer: reached {MAX_SLOTS} slots — done")
        api.stop()
        return
    _window_slots += 1
    _window_bytes += _slot_bytes

    _last_count = int(in_slot.count)
    _scale = float(in_slot.scale)
    if abs(_scale - EXPECTED_SCALE) > 1e-5:
        _bad_scale += 1

    if _received > 1:
        gap = _last_count - _prev_count - 1
        if gap > 0:
            _overflow_events += 1
            _overflow_slots += gap
            _window_overflow_events += 1
            _window_overflow_slots += gap
    _prev_count = _last_count

    if np is not None:
        arr = np.ctypeslib.as_array(in_slot.samples)
        _sample0 = float(arr[0])
        _sample_last = float(arr[-1])
    else:
        _sample0 = float(in_slot.samples[0])
        _sample_last = float(in_slot.samples[BLOCK_SIZE - 1])

    now = time.time()
    dt = now - _window_start
    if dt >= 1.0:
        slots_per_sec = _window_slots / dt
        mib_per_sec = (_window_bytes / (1024.0 * 1024.0)) / dt
        expected0 = EXPECTED_SCALE * _last_count
        expected_last = EXPECTED_SCALE * (_last_count + BLOCK_SIZE - 1)
        ok0 = abs(_sample0 - expected0) < 1.0
        ok_last = abs(_sample_last - expected_last) < 1.0
        correctness = "OK" if (ok0 and ok_last and _bad_scale == 0) else "MISMATCH"
        print(
            f"throughput: slots/s={slots_per_sec:<9.1f} "
            f"MiB/s={mib_per_sec:<9.2f} "
            f"total={_received:<10d} "
            f"count={_last_count:<10d} "
            f"scale={_scale:<.1f} "
            f"overflow(ev/sl)={_window_overflow_events}/{_window_overflow_slots} "
            f"data={correctness}",
            flush=True,
        )
        _window_slots = 0
        _window_bytes = 0
        _window_overflow_events = 0
        _window_overflow_slots = 0
        _window_start = now


def on_stop(api: cons.ConsumerAPI) -> None:
    api.log(
        "info",
        f"DemoConsumer: stopped total_received={_received} "
        f"overflow_events={_overflow_events} overflow_slots={_overflow_slots} "
        f"bad_scale={_bad_scale}",
    )
