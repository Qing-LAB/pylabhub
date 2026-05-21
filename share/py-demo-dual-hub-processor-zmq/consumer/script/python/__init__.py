"""Dual-hub demo consumer.

Reads lab.demo.processed on hub-b.  Logs running stats from the
flexzone (which the processor updates per slot).  No explicit
shutdown logic — the default `on_channel_closing` callback fires
when hub-b notifies that the channel closed (because the processor
stopped, which happened because hub-a notified that producer
stopped, which happened because producer hit its slot target).
"""

import time

BLOCK_SIZE = 1024

_received = 0
_t0 = 0.0
_fz = None


def on_init(api) -> None:
    global _t0, _fz
    _t0 = time.time()
    _fz = api.flexzone()
    api.log("info",
            f"DualCons started uid={api.uid()} channel={api.channel()} "
            f"(reading from hub-b)")


def on_consume(rx, messages, api) -> bool:
    global _received
    if rx.slot is None:
        return True
    _received += 1
    if _received % 1000 == 0:
        elapsed = time.time() - _t0
        rate = _received / max(elapsed, 1e-9)
        fz_line = ""
        if _fz is not None:
            fz_line = (f" | fz: total={_fz.total_slots} "
                       f"running_mean={_fz.running_mean:.4f}")
        api.log("info",
                f"DualCons received count={rx.slot.count} "
                f"slot_mean={rx.slot.slot_mean:.4f} "
                f"rate={rate:.0f} slots/s{fz_line}")
    return True


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    rate = _received / elapsed
    api.log("info",
            f"DualCons STOPPED total_received={_received} "
            f"avg_rate={rate:.0f} slots/s "
            f"(stopped via cross-hub channel-close cascade)")
