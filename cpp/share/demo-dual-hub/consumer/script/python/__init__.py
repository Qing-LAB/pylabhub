"""
Dual-hub demo consumer — reads 'lab.bridge.processed' from Hub B.

Slot: count(int64), ts(float64), value(float32), doubled(float32)
"""

import time

import pylabhub_consumer as cons

_received: int   = 0
_last_ts:  float = 0.0


def on_init(api: cons.ConsumerAPI) -> None:
    api.log('info', f"BridgeConsumer: started  uid={api.uid()}")


def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:
    global _received, _last_ts

    if in_slot is None:
        api.log('debug', "BridgeConsumer: timeout")
        return

    _received += 1
    now = time.time()
    rate = 1.0 / (now - _last_ts) if _last_ts > 0 else 0.0
    _last_ts = now

    if _received % 50 == 0:
        api.log('info',
                f"BridgeConsumer: count={in_slot.count}"
                f"  value={in_slot.value:.3f}"
                f"  doubled={in_slot.doubled:.3f}"
                f"  rate={rate:.1f} Hz"
                f"  total={_received}")
    else:
        print(f"  count={in_slot.count:6d}  ts={in_slot.ts:.3f}"
              f"  value={in_slot.value:8.3f}  doubled={in_slot.doubled:8.3f}"
              f"  rate={rate:5.1f} Hz",
              flush=True)


def on_stop(api: cons.ConsumerAPI) -> None:
    api.log('info', f"BridgeConsumer: stopped  total_received={_received}")
