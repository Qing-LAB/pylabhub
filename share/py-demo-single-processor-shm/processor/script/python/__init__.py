"""Throughput processor.

For each input slot:
- Compute per-slot stats (mean, std, min, max) via numpy on the 4096-sample
  input block.
- Write a "processed" output block (here: input × 2.0) to the output slot.
- Update the OUTPUT flexzone running stats (total_slots, running_mean,
  running_min, running_max) so the consumer can see cumulative stats.

The processor is the heavyweight stage — numpy ops on 4096 floats per
slot at max_rate is the realistic throughput-stress workload.
"""

import time

import numpy as np

BLOCK_SIZE = 4096
SCALE = 2.0

_processed = 0
_t0 = 0.0
_fz = None  # cached pointer to output flexzone (initialised lazily; first
            # call to api.flexzone() returns the ctypes struct view)


def on_init(api) -> None:
    global _t0, _fz
    _t0 = time.time()
    _fz = api.flexzone(api.Tx)  # OUTPUT flexzone (writable ctypes view)
    if _fz is not None:
        _fz.total_slots  = 0
        _fz.running_mean = 0.0
        _fz.running_min  = float("inf")
        _fz.running_max  = float("-inf")
        api.update_flexzone_checksum()
    api.log("info",
            f"DemoProcessor started uid={api.uid()} "
            f"{api.in_channel()} -> {api.out_channel()} "
            f"block={BLOCK_SIZE} SCALE={SCALE}")


def on_process(rx, tx, messages, api) -> bool:
    global _processed
    if rx.slot is None or tx.slot is None:
        return False

    # Zero-copy numpy views of input + output blocks.
    in_arr  = api.as_numpy(rx.slot.samples)
    out_arr = api.as_numpy(tx.slot.processed)

    # Per-slot stats (numpy reductions on 4096 floats).
    s_mean = float(in_arr.mean())
    s_std  = float(in_arr.std())
    s_min  = float(in_arr.min())
    s_max  = float(in_arr.max())

    # Process: out = in × SCALE (zero-copy scale into output buffer).
    np.multiply(in_arr, np.float32(SCALE), out=out_arr)

    # Per-slot metadata.
    tx.slot.count     = rx.slot.count
    tx.slot.ts        = rx.slot.ts
    tx.slot.slot_mean = s_mean
    tx.slot.slot_std  = s_std
    tx.slot.slot_min  = s_min
    tx.slot.slot_max  = s_max

    # Update flexzone running stats — incremental mean over total slots.
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
