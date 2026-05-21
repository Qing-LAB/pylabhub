"""Single-hub demo consumer.

Reads lab.demo.processed and logs each received slot's count + value +
doubled.  Logs every 10th slot to keep the harness output digestible.
"""

import time

_received: int = 0
_t0: float = 0.0


def on_init(api) -> None:
    global _t0
    _t0 = time.time()
    api.log("info", f"DemoConsumer started uid={api.uid()} channel={api.channel()}")


def on_consume(rx, messages, api) -> bool:
    global _received
    if rx.slot is None:
        return True
    _received += 1
    if _received % 10 == 0:
        api.log("info",
                f"DemoConsumer received count={rx.slot.count} "
                f"value={rx.slot.value:.4f} doubled={rx.slot.doubled:.4f}")
    return True


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    api.log("info",
            f"DemoConsumer stopped total_received={_received} "
            f"avg_rate={_received/elapsed:.1f} Hz")
