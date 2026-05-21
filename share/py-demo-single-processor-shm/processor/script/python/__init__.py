"""Single-hub demo processor.

Reads lab.demo.counter, writes lab.demo.processed with added
`doubled = value * 2.0`.  Used by the demo framework manifest to
exercise the producer→processor→consumer chain.
"""

import time

_processed: int = 0
_t0: float = 0.0


def on_init(api) -> None:
    global _t0
    _t0 = time.time()
    api.log("info",
            f"DemoProcessor started uid={api.uid()} "
            f"{api.in_channel()} -> {api.out_channel()}")


def on_process(rx, tx, messages, api) -> bool:
    global _processed
    if rx.slot is None or tx.slot is None:
        return False
    tx.slot.count   = rx.slot.count
    tx.slot.ts      = rx.slot.ts
    tx.slot.value   = rx.slot.value
    tx.slot.doubled = float(rx.slot.value) * 2.0
    _processed += 1
    if _processed % 10 == 0:
        api.log("info",
                f"DemoProcessor processed count={rx.slot.count} "
                f"doubled={tx.slot.doubled:.4f}")
    return True


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    api.log("info",
            f"DemoProcessor stopped processed={_processed} "
            f"avg_rate={_processed/elapsed:.1f} Hz "
            f"in={api.in_slots_received()} out={api.out_slots_written()}")
