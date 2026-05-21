"""Dual-hub demo producer.

Writes lab.demo.counter on hub-a (100 Hz, 1024-float32 samples per
slot).  After SLOT_TARGET slots, calls api.stop() to initiate the
shutdown cascade: producer.stop → hub-a sends CHANNEL_CLOSING_NOTIFY
→ processor's consumer-side default on_channel_closing fires →
processor.stop → hub-b sends CHANNEL_CLOSING_NOTIFY → consumer's
default on_channel_closing fires → consumer.stop.  This exercises
the cross-hub channel-close cascade (D1/D2 fix this session) without
needing federation-relayed bands.
"""

import math
import time

import numpy as np

BLOCK_SIZE = 1024
SLOT_TARGET = 1000   # producer initiates shutdown after N slots (~10s at 100 Hz)
PHASE_STEP = 2.0 * math.pi / 64.0

_count = 0
_t0 = 0.0
_stopped = False
_base = np.arange(BLOCK_SIZE, dtype=np.float32) * np.float32(PHASE_STEP)


def on_init(api) -> None:
    global _t0
    _t0 = time.time()
    api.log("info",
            f"DualProd started uid={api.uid()} channel={api.channel()} "
            f"target={SLOT_TARGET} slots (then stop → cross-hub cascade)")


def on_produce(tx, messages, api) -> bool:
    global _count, _stopped
    if tx.slot is None:
        return False
    _count += 1
    tx.slot.count = _count
    tx.slot.ts    = time.time()
    arr = api.as_numpy(tx.slot.samples)
    arr[:] = np.sin(_base + np.float32(_count * 0.01))
    if _count % 1000 == 0:
        elapsed = time.time() - _t0
        api.log("info",
                f"DualProd wrote count={_count} "
                f"rate={_count/max(elapsed,1e-9):.0f} slots/s")
    if not _stopped and _count >= SLOT_TARGET:
        _stopped = True
        api.log("info",
                f"DualProd reached target={SLOT_TARGET} — calling "
                f"api.stop() to initiate cross-hub close cascade")
        api.stop()
    return True


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    api.log("info",
            f"DualProd STOPPED total={_count} "
            f"avg_rate={_count/elapsed:.0f} slots/s")
