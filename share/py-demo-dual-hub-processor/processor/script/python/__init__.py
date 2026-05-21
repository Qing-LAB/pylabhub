"""Dual-hub demo processor.

Consumer-side presence on hub-a (in_channel=lab.demo.counter).
Producer-side presence on hub-b (out_channel=lab.demo.processed).
This is the Wave-B M8 dual-hub payoff: ONE processor process with
TWO presences, registering on TWO brokers, with one ctrl thread per
unique hub.

For each input slot:
- numpy mean / std reductions on the 1024-sample input.
- Write the input × 2.0 to the output's `processed` field.
- Update the OUTPUT flexzone running stats.

No explicit shutdown logic — the default `on_channel_closing`
callback (D1/D2 fix shipped this session) calls api.stop() when the
input channel closes (producer stopped on hub-a) OR when the output
channel closes (consumer stopped on hub-b).
"""

import time

import numpy as np

BLOCK_SIZE = 1024
SCALE = 2.0

_processed = 0
_t0 = 0.0
_fz = None


def on_init(api) -> None:
    global _t0, _fz
    _t0 = time.time()
    _fz = api.flexzone(api.Tx)
    if _fz is not None:
        _fz.total_slots  = 0
        _fz.running_mean = 0.0
        api.update_flexzone_checksum()
    api.log("info",
            f"DualProc started uid={api.uid()} "
            f"{api.in_channel()} (hub-a) -> {api.out_channel()} (hub-b) "
            f"block={BLOCK_SIZE}")


def on_process(rx, tx, messages, api) -> bool:
    global _processed
    if rx.slot is None or tx.slot is None:
        return False
    in_arr  = api.as_numpy(rx.slot.samples)
    out_arr = api.as_numpy(tx.slot.processed)
    s_mean = float(in_arr.mean())
    s_std  = float(in_arr.std())
    np.multiply(in_arr, np.float32(SCALE), out=out_arr)
    tx.slot.count     = rx.slot.count
    tx.slot.ts        = rx.slot.ts
    tx.slot.slot_mean = s_mean
    tx.slot.slot_std  = s_std
    if _fz is not None:
        n = _fz.total_slots + 1
        _fz.running_mean = (_fz.running_mean * (n - 1) + s_mean) / n
        _fz.total_slots = n
        api.update_flexzone_checksum()
    _processed += 1
    if _processed % 1000 == 0:
        elapsed = time.time() - _t0
        api.log("info",
                f"DualProc processed count={rx.slot.count} "
                f"rate={_processed/max(elapsed,1e-9):.0f} slots/s "
                f"mean={s_mean:.4f}")
    return True


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    api.log("info",
            f"DualProc STOPPED processed={_processed} "
            f"avg_rate={_processed/elapsed:.0f} slots/s "
            f"(stopped via cross-hub channel-close cascade)")
