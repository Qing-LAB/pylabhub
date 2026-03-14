"""Bridge B: transform — receives 1k-sample blocks via ZMQ, doubles all samples, writes SHM.

Sets scale=2.0 in the output slot so the consumer can verify correctness.
Stops when upstream channel_closing arrives, which closes the SHM output and
triggers channel_closing in the Consumer.

Pipeline: Producer [SHM] -> Bridge A [ZMQ PUSH] -> Bridge B [ZMQ PULL] -> [SHM] -> Consumer
"""

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
        f"BridgeB: started uid={api.uid()} "
        f"{api.in_channel()} -> {api.out_channel()} (ZMQ PULL -> SHM)",
    )


def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
    global _processed

    for msg in messages:
        if isinstance(msg, dict) and msg.get("event") == "channel_closing":
            api.log("info", "BridgeB: upstream channel closed — stopping")
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
        f"BridgeB: stopped processed={_processed} avg_rate={_processed/elapsed:.1f} Hz "
        f"in={api.in_slots_received()} out={api.out_slots_written()} drops={api.out_drop_count()}",
    )
