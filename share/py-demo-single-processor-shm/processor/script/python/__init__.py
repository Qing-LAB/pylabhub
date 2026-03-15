"""Throughput processor: doubles all 1024 float32 samples, sets scale=2.0.
Stops automatically when the upstream channel_closing event arrives."""

import time

import pylabhub_processor as proc

try:
    import numpy as np
except Exception:
    np = None

BLOCK_SIZE = 1024
SCALE = 2.0

_processed: int = 0
_start: float = 0.0


def on_init(api: proc.ProcessorAPI) -> None:
    global _start
    _start = time.time()
    api.log(
        "info",
        f"DemoDoubler: started uid={api.uid()} {api.in_channel()} -> {api.out_channel()}",
    )


def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
    global _processed

    # Propagate shutdown: when upstream producer stops, chain the stop downstream.
    for msg in messages:
        if isinstance(msg, dict) and msg.get("event") == "channel_closing":
            api.log("info", "DemoDoubler: upstream channel closed — stopping")
            api.stop()
            return False

    if in_slot is None:
        return False

    out_slot.count = in_slot.count
    out_slot.ts = in_slot.ts
    out_slot.scale = SCALE

    if np is not None:
        arr_in = np.ctypeslib.as_array(in_slot.samples)
        arr_out = np.ctypeslib.as_array(out_slot.samples)
        arr_out[:] = arr_in * np.float32(SCALE)
    else:
        for i in range(BLOCK_SIZE):
            out_slot.samples[i] = float(in_slot.samples[i]) * SCALE

    _processed += 1
    return True


def on_stop(api: proc.ProcessorAPI) -> None:
    elapsed = max(time.time() - _start, 1e-9)
    api.log(
        "info",
        f"DemoDoubler: stopped processed={_processed} avg_rate={_processed/elapsed:.1f} Hz "
        f"in={api.in_slots_received()} out={api.out_slots_written()} drops={api.out_drop_count()}",
    )
