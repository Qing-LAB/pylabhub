"""Single-hub demo producer.

Writes lab.demo.counter at 10 Hz: count (monotonic), ts (Unix time),
value (sine wave).  Used by the demo framework's manifest to drive
binary-level validation of the producer-processor-consumer pipeline.
"""

import math
import time

_count: int = 0
_t0: float = 0.0


def on_init(api) -> None:
    global _t0
    _t0 = time.time()
    api.log("info", f"DemoProducer started uid={api.uid()} channel={api.channel()}")


def on_produce(tx, messages, api) -> bool:
    global _count
    if tx.slot is None:
        return False
    _count += 1
    tx.slot.count = _count
    tx.slot.ts    = time.time()
    tx.slot.value = float(math.sin(_count * 0.1))
    if _count % 10 == 0:
        api.log("info", f"DemoProducer wrote slot count={_count}")
    return True


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    api.log("info",
            f"DemoProducer stopped total={_count} avg_rate={_count/elapsed:.1f} Hz")
