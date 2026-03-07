"""
Dual-hub demo producer — publishes counter on 'lab.bridge.raw' at 10 Hz.

Slot: count(int64), ts(float64), value(float32)
"""

import math
import time

import pylabhub_producer as prod

_count: int = 0


def on_init(api: prod.ProducerAPI) -> None:
    api.log('info', f"BridgeProducer: started  uid={api.uid()}")


def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:
    global _count

    if out_slot is None:
        return False

    _count += 1
    out_slot.count = _count
    out_slot.ts    = time.time()
    out_slot.value = math.sin(_count * 0.1) * 10.0

    if _count % 50 == 0:
        api.log('info', f"BridgeProducer: count={_count}  value={out_slot.value:.3f}")
    return True


def on_stop(api: prod.ProducerAPI) -> None:
    api.log('info', f"BridgeProducer: stopped  total={_count}")
