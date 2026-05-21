"""Throughput consumer.

For each input slot, accumulate count + bytes for rate reporting.  Every
5000th slot, log per-slot stats AND the flexzone's running cumulative
stats (visible as rx.fz, zero-copy read-only view per HEP-CORE-0019
§8.4).
"""

import time

BLOCK_SIZE = 4096
SLOT_BYTES_IN = 8 + 8 + 4 * BLOCK_SIZE + 8 * 4   # count+ts+processed[]+4*float64
                                                  # = 16432 bytes per slot

_received = 0
_t0 = 0.0
_fz = None  # rx flexzone view; doc §8.4 says rx.fz, but the
            # ConsumerAPI/RxChannel binding doesn't expose .fz — use
            # api.flexzone() instead.  Audit B6 (2026-05-21).


def on_init(api) -> None:
    global _t0, _fz
    _t0 = time.time()
    _fz = api.flexzone()  # read-only view of the producer's (here:
                           # processor's) flexzone
    api.log("info",
            f"DemoConsumer started uid={api.uid()} channel={api.channel()} "
            f"slot_bytes={SLOT_BYTES_IN}")


def on_consume(rx, messages, api) -> bool:
    global _received
    if rx.slot is None:
        return True
    _received += 1
    if _received % 5000 == 0:
        elapsed = time.time() - _t0
        rate = _received / max(elapsed, 1e-9)
        mib  = _received * SLOT_BYTES_IN / (1024 * 1024) / max(elapsed, 1e-9)
        fz_line = ""
        if _fz is not None:
            fz_line = (f" | flexzone running: total={_fz.total_slots} "
                       f"mean={_fz.running_mean:.4f} "
                       f"min={_fz.running_min:.4f} "
                       f"max={_fz.running_max:.4f}")
        api.log("info",
                f"DemoConsumer received count={rx.slot.count} "
                f"slot_mean={rx.slot.slot_mean:.4f} "
                f"rate={rate:.0f} slots/s throughput={mib:.1f} MiB/s"
                f"{fz_line}")
    return True


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    rate = _received / elapsed
    mib  = _received * SLOT_BYTES_IN / (1024 * 1024) / elapsed
    m = api.metrics()
    loop = m.get("loop", {}) or {}
    q    = m.get("queue", {}) or {}
    role = m.get("role", {}) or {}
    iters = loop.get("iteration_count", 0)
    api.log("info",
            f"DemoConsumer STOPPED total_received={_received} "
            f"avg_rate={rate:.0f} slots/s "
            f"avg_throughput={mib:.1f} MiB/s "
            f"| iters={iters} ({iters/elapsed:.0f}/s) "
            f"work_us_last={loop.get('last_cycle_work_us','n/a')} "
            f"acquire_retries={loop.get('acquire_retry_count','n/a')} "
            f"slot_wait_us_last={q.get('last_slot_wait_us','n/a')} "
            f"in_received={role.get('in_slots_received','n/a')} "
            f"last_seq={api.last_seq()}")
