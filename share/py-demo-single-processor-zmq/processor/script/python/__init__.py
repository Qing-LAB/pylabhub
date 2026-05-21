"""Throughput processor with band-coordinated shutdown.

Reads producer's 4096-float32 blocks; computes per-slot stats
(mean/std/min/max) via numpy; multiplies the block by 2.0 (zero-copy
np.multiply with `out=`); writes processed slot + running stats in
the OUTPUT flexzone.

Coordinated shutdown: joins `!demo.shutdown` band in on_init; on
receiving a `b"drain"` band message, calls api.stop() to exit
cleanly.
"""

import time

import numpy as np

BLOCK_SIZE = 4096
SCALE = 2.0
SHUTDOWN_BAND = "!demo.shutdown"

_processed = 0
_t0 = 0.0
_fz = None


_band_joined = False


def on_init(api) -> None:
    global _t0, _fz
    _t0 = time.time()
    _fz = api.flexzone(api.Tx)
    if _fz is not None:
        _fz.total_slots  = 0
        _fz.running_mean = 0.0
        _fz.running_min  = float("inf")
        _fz.running_max  = float("-inf")
        api.update_flexzone_checksum()
    # band_join deferred to on_process (handler not yet up here —
    # see HEP-CORE-0011 §"Initialization Protocol").
    api.log("info",
            f"DemoProcessor started uid={api.uid()} "
            f"{api.in_channel()} -> {api.out_channel()} "
            f"block={BLOCK_SIZE} SCALE={SCALE} band={SHUTDOWN_BAND}")


def on_process(rx, tx, messages, api) -> bool:
    global _processed, _band_joined
    if not _band_joined:
        res = api.band_join(SHUTDOWN_BAND)
        if res is not None and res.get("status") == "success":
            _band_joined = True
            api.log("info", f"DemoProcessor joined band '{SHUTDOWN_BAND}'")
        else:
            api.log("warn", f"DemoProcessor band_join('{SHUTDOWN_BAND}') failed: {res}")
            _band_joined = True
    if rx.slot is None or tx.slot is None:
        return False

    in_arr  = api.as_numpy(rx.slot.samples)
    out_arr = api.as_numpy(tx.slot.processed)

    s_mean = float(in_arr.mean())
    s_std  = float(in_arr.std())
    s_min  = float(in_arr.min())
    s_max  = float(in_arr.max())

    np.multiply(in_arr, np.float32(SCALE), out=out_arr)

    tx.slot.count     = rx.slot.count
    tx.slot.ts        = rx.slot.ts
    tx.slot.slot_mean = s_mean
    tx.slot.slot_std  = s_std
    tx.slot.slot_min  = s_min
    tx.slot.slot_max  = s_max

    if _fz is not None:
        n = _fz.total_slots + 1
        _fz.running_mean = (_fz.running_mean * (n - 1) + s_mean) / n
        if s_min < _fz.running_min: _fz.running_min = s_min
        if s_max > _fz.running_max: _fz.running_max = s_max
        _fz.total_slots = n
        api.update_flexzone_checksum()

    _processed += 1
    if _processed % 5000 == 0:
        elapsed = time.time() - _t0
        api.log("info",
                f"DemoProcessor processed count={rx.slot.count} "
                f"rate={_processed/max(elapsed,1e-9):.0f} slots/s "
                f"slot_mean={s_mean:.4f} slot_std={s_std:.4f}")
    return True


def on_band_message(band, sender, body, api) -> None:
    if band == SHUTDOWN_BAND and isinstance(body, dict) and body.get("cmd") == "drain":
        api.log("info",
                f"DemoProcessor received 'drain' on '{band}' from "
                f"'{sender}' — stopping cleanly")
        api.stop()


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    m = api.metrics()
    loop = m.get("loop", {}) or {}
    iq   = m.get("in_queue", {}) or {}
    oq   = m.get("out_queue", {}) or {}
    role = m.get("role", {}) or {}
    iters = loop.get("iteration_count", 0)
    api.log("info",
            f"DemoProcessor STOPPED processed={_processed} "
            f"avg_rate={_processed/elapsed:.0f} slots/s "
            f"in_throughput={_processed*BLOCK_SIZE*4/(1024*1024)/elapsed:.1f} MiB/s "
            f"| iters={iters} ({iters/elapsed:.0f}/s) "
            f"work_us_last={loop.get('last_cycle_work_us','n/a')} "
            f"overruns={loop.get('loop_overrun_count','n/a')} "
            f"acquire_retries={loop.get('acquire_retry_count','n/a')} "
            f"in_wait_us_last={iq.get('last_slot_wait_us','n/a')} "
            f"out_wait_us_last={oq.get('last_slot_wait_us','n/a')} "
            f"in_received={role.get('in_slots_received','n/a')} "
            f"out_written={role.get('out_slots_written','n/a')} "
            f"drops={role.get('out_drop_count','n/a')}")
