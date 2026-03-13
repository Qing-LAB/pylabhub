"""Bridge A: pass-through — copies 1k-sample SHM input to ZMQ output unchanged.

Stops when upstream channel_closing arrives, which closes the ZMQ PUSH socket
and triggers a channel_closing event in Bridge B.

Pipeline: Producer [SHM] -> Bridge A [ZMQ PUSH] -> Bridge B [ZMQ PULL] -> [SHM] -> Consumer
"""

import time

import pylabhub_processor as proc

try:
    import numpy as np
except Exception:
    np = None

BLOCK_SIZE = 1024

_processed: int = 0
_start: float = 0.0


def on_init(api: proc.ProcessorAPI) -> None:
    global _start
    _start = time.time()
    api.log(
        "info",
        f"BridgeA: started uid={api.uid()} "
        f"{api.in_channel()} -> {api.out_channel()} (SHM -> ZMQ PUSH)",
    )


def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
    global _processed

    for msg in messages:
        if isinstance(msg, dict) and msg.get("event") == "channel_closing":
            api.log("info", "BridgeA: upstream channel closed — stopping")
            api.stop()
            return False

    if in_slot is None:
        return False

    out_slot.count = in_slot.count
    out_slot.ts = in_slot.ts

    if np is not None:
        arr_in = np.ctypeslib.as_array(in_slot.samples)
        arr_out = np.ctypeslib.as_array(out_slot.samples)
        arr_out[:] = arr_in
    else:
        for i in range(BLOCK_SIZE):
            out_slot.samples[i] = in_slot.samples[i]

    _processed += 1
    return True


def on_stop(api: proc.ProcessorAPI) -> None:
    elapsed = max(time.time() - _start, 1e-9)
    api.log(
        "info",
        f"BridgeA: stopped processed={_processed} avg_rate={_processed/elapsed:.1f} Hz "
        f"in={api.in_slots_received()} out={api.out_slots_written()} drops={api.out_drop_count()}",
    )
