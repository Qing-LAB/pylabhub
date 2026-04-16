"""Throughput producer: emits count + ts + 1024 float32 samples at max rate.
Runs until stopped externally (e.g. by the consumer reaching its limit)."""

import time

import pylabhub_producer as prod

try:
    import numpy as np
except Exception:
    np = None

BLOCK_SIZE = 1024

_count: int = 0
_start: float = 0.0
_base = None


def on_init(api: prod.ProducerAPI) -> None:
    global _start, _base
    _start = time.time()
    if np is not None:
        _base = np.arange(BLOCK_SIZE, dtype=np.float32)
    api.log(
        "info",
        f"DemoProducer: started uid={api.uid()} "
        f"block={BLOCK_SIZE} float32  max_rate",
    )


def on_produce(tx, messages, api: prod.ProducerAPI) -> bool:
    global _count

    # Handle inbox messages (tuples only; ignore event dicts).
    for msg in messages:
        if isinstance(msg, tuple):
            sender, data = msg
            api.log("debug", f"DemoProducer: ctrl from {sender!r}: {data!r}")

    if tx.slot is None:
        return False

    _count += 1
    tx.slot.count = _count
    tx.slot.ts = time.time()

    if np is not None:
        arr = np.ctypeslib.as_array(tx.slot.samples)
        arr[:] = _base + np.float32(_count)
    else:
        for i in range(BLOCK_SIZE):
            tx.slot.samples[i] = float(_count + i)

    return True


def on_stop(api: prod.ProducerAPI) -> None:
    elapsed = max(time.time() - _start, 1e-9)
    api.log(
        "info",
        f"DemoProducer: stopped total={_count} avg_rate={_count/elapsed:.1f} Hz "
        f"loop_overruns={api.loop_overrun_count()}",
    )
