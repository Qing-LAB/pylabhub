"""Throughput producer.

Writes count + ts + samples(4096 float32) per slot at max_rate.
Samples are a sine wave with per-slot phase offset (cheap to generate;
the goal is throughput, not signal complexity).
"""

import math
import time

import numpy as np

BLOCK_SIZE = 4096
PHASE_STEP = 2.0 * math.pi / 256.0

_count = 0
_t0 = 0.0
_base = np.arange(BLOCK_SIZE, dtype=np.float32) * np.float32(PHASE_STEP)


def on_init(api) -> None:
    global _t0
    _t0 = time.time()
    api.log("info",
            f"DemoProducer started uid={api.uid()} channel={api.channel()} "
            f"block={BLOCK_SIZE} float32 max_rate")


def on_produce(tx, messages, api) -> bool:
    global _count
    if tx.slot is None:
        return False
    _count += 1
    tx.slot.count = _count
    tx.slot.ts    = time.time()
    arr = api.as_numpy(tx.slot.samples)
    arr[:] = np.sin(_base + np.float32(_count * 0.01))
    if _count % 5000 == 0:
        elapsed = time.time() - _t0
        api.log("info",
                f"DemoProducer wrote slot count={_count} "
                f"rate={_count/max(elapsed, 1e-9):.0f} slots/s "
                f"throughput={_count*BLOCK_SIZE*4/(1024*1024)/max(elapsed,1e-9):.1f} MiB/s")
    return True


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    api.log("info",
            f"DemoProducer STOPPED total={_count} "
            f"avg_rate={_count/elapsed:.0f} slots/s "
            f"avg_throughput={_count*BLOCK_SIZE*4/(1024*1024)/elapsed:.1f} MiB/s")
