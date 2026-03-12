"""
Dual-hub demo producer — publishes counter on 'lab.bridge.raw'.
Runs until stopped externally (e.g. by the consumer reaching its limit).

Slot: count(int64), ts(float64), value(float32)
"""

import math
import time

import pylabhub_producer as prod

_count: int = 0
_start: float = 0.0


def on_init(api: prod.ProducerAPI) -> None:
    global _start
    _start = time.time()
    api.log('info', f"BridgeProducer: started  uid={api.uid()}")


def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:
    global _count

    # Handle inbox messages.
    for msg in messages:
        if isinstance(msg, tuple):
            sender, data = msg
            api.log('debug', f"BridgeProducer: ctrl from {sender!r}: {data!r}")

    if out_slot is None:
        return False

    _count += 1
    out_slot.count = _count
    out_slot.ts    = time.time()
    out_slot.value = math.sin(_count * 0.1) * 10.0

    return True


def on_stop(api: prod.ProducerAPI) -> None:
    elapsed = max(time.time() - _start, 1e-9)
    api.log('info', f"BridgeProducer: stopped  total={_count}  avg_rate={_count/elapsed:.1f} Hz")
