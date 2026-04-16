"""Throughput consumer for dual-hub demo: reports slots/s, MB/s, wait/work times, and
correctness every second. Reads from Hub B (lab.bridge.processed).

max_slots is read from consumer.json ("max_slots" field, default 1000000).
Set to 0 for unlimited (run until Ctrl-C).

Queue occupancy proxy in throughput line:
  wait_us  — avg μs the consumer spent waiting for the next slot per window.
             ≈ 0  → slot was already buffered (ring non-empty, consumer not faster than producer)
             > 0  → ring drained; consumer waited for new data (consumer keeping up or ahead)
  work_us  — avg μs spent inside on_consume per slot (numpy + ctypes overhead)

Slot: count(int64), ts(float64), samples(float32×1024), scale(float32)
"""

import ctypes
import json
import os
import time

import pylabhub_consumer as cons

try:
    import numpy as np
except Exception:
    np = None

BLOCK_SIZE = 1024
EXPECTED_SCALE = 2.0
MAX_SLOTS = 1000000  # overridden in on_init from consumer.json

_received: int = 0
_window_slots: int = 0
_window_bytes: int = 0
_window_wait_us: int = 0
_window_work_us: int = 0
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
    global _window_start, MAX_SLOTS
    _window_start = time.time()

    cfg_path = os.path.join(api.role_dir(), "consumer.json")
    try:
        with open(cfg_path) as f:
            cfg = json.load(f)
        MAX_SLOTS = int(cfg.get("max_slots", 1000000))
    except Exception:
        MAX_SLOTS = 1000000

    limit_str = str(MAX_SLOTS) if MAX_SLOTS > 0 else "unlimited (Ctrl-C to stop)"
    api.log("info", f"BridgeConsumer: started uid={api.uid()} throughput window=1s max_slots={limit_str}")


def on_consume(rx, messages, api: cons.ConsumerAPI) -> None:
    global _received, _window_slots, _window_bytes, _window_wait_us, _window_work_us
    global _window_start, _slot_bytes
    global _last_count, _sample0, _sample_last, _scale
    global _prev_count, _overflow_events, _overflow_slots
    global _window_overflow_events, _window_overflow_slots, _bad_scale

    # Propagate shutdown: when Bridge B stops, the channel closes here too.
    for msg in messages:
        if isinstance(msg, dict) and msg.get("event") == "channel_closing":
            api.log("info", "BridgeConsumer: channel closed — stopping")
            api.stop()
            return

    if rx.slot is None:
        return

    if _slot_bytes == 0:
        _slot_bytes = ctypes.sizeof(type(rx.slot))
        api.log(
            "info",
            f"BridgeConsumer: slot_bytes={_slot_bytes} payload={BLOCK_SIZE}xfloat32 + scale:float32",
        )

    _received += 1
    if MAX_SLOTS > 0 and _received >= MAX_SLOTS:
        api.log("info", f"BridgeConsumer: reached {MAX_SLOTS} slots — done")
        api.stop()
        return

    _window_slots += 1
    _window_bytes += _slot_bytes

    m = api.metrics()
    _window_wait_us += int(m.get("last_slot_wait_us", 0))
    _window_work_us += int(m.get("last_slot_work_us", 0))

    _last_count = int(rx.slot.count)
    _scale = float(rx.slot.scale)
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
        arr = np.ctypeslib.as_array(rx.slot.samples)
        _sample0 = float(arr[0])
        _sample_last = float(arr[-1])
    else:
        _sample0 = float(rx.slot.samples[0])
        _sample_last = float(rx.slot.samples[BLOCK_SIZE - 1])

    now = time.time()
    dt = now - _window_start
    if dt >= 1.0:
        slots_per_sec = _window_slots / dt
        mib_per_sec = (_window_bytes / (1024.0 * 1024.0)) / dt
        avg_wait_us = _window_wait_us / max(_window_slots, 1)
        avg_work_us = _window_work_us / max(_window_slots, 1)
        # Verify: samples[i] = EXPECTED_SCALE * (count + i), scale == EXPECTED_SCALE
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
            f"wait_us={avg_wait_us:<7.1f} "
            f"work_us={avg_work_us:<7.1f} "
            f"data={correctness}",
            flush=True,
        )
        _window_slots = 0
        _window_bytes = 0
        _window_wait_us = 0
        _window_work_us = 0
        _window_overflow_events = 0
        _window_overflow_slots = 0
        _window_start = now


def on_stop(api: cons.ConsumerAPI) -> None:
    api.log(
        "info",
        f"BridgeConsumer: stopped total_received={_received} "
        f"overflow_events={_overflow_events} overflow_slots={_overflow_slots} "
        f"bad_scale={_bad_scale}",
    )
