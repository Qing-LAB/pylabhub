"""Engine-throughput bench: Python producer.

Band-coordinated shutdown (HEP-CORE-0030):
- Lazy band_join("!bench.shutdown") on first on_produce (handler not
  up at on_init time — see HEP-0011 API-availability note).
- At MEASUREMENT_S elapsed, broadcast {"cmd":"drain"} + api.stop().
- Consumer's on_band_message receives the drain and calls api.stop().

Warmup snapshotting separates transient from steady-state rate.
"""

import time

import numpy as np

ENGINE         = "python"
SHUTDOWN_BAND  = "!bench.shutdown"
WARMUP_S       = 5.0
MEASUREMENT_S  = 40.0
PAYLOAD_LEN    = 1020   # float32 — matches producer.json schema

_count = 0
_t0 = 0.0
_warmup_count = -1
_warmup_t = 0.0
_band_joined = False
_drain_sent = False
_rng = np.random.default_rng(0)


def on_init(api) -> None:
    global _t0
    _t0 = time.perf_counter()
    api.log("info", f"BenchProd-Python started warmup_s={WARMUP_S} measurement_s={MEASUREMENT_S}")


def on_produce(tx, messages, api) -> bool:
    global _count, _warmup_count, _warmup_t, _band_joined, _drain_sent
    if not _band_joined:
        res = api.band_join(SHUTDOWN_BAND)
        _band_joined = True  # don't retry forever
        if res is not None and res.get("status") == "success":
            api.log("info", f"BenchProd joined band '{SHUTDOWN_BAND}'")
    if tx.slot is None:
        return False
    _count += 1
    tx.slot.count = _count
    tx.slot.value = _count * 0.5
    # Fresh random numbers each slot — fills the 4 KB payload via
    # numpy zero-copy view onto the SHM block.
    arr = api.as_numpy(tx.slot.payload)
    arr[:] = _rng.random(PAYLOAD_LEN, dtype=np.float32)
    # Periodic clock check (every 1024 slots).  Snapshot warmup boundary
    # + trigger shutdown at MEASUREMENT_S.
    if (_count & 0x3FF) == 0:
        elapsed = time.perf_counter() - _t0
        if _warmup_count < 0 and elapsed >= WARMUP_S:
            _warmup_count = _count
            _warmup_t = time.perf_counter()
        if not _drain_sent and elapsed >= MEASUREMENT_S:
            _drain_sent = True
            api.log("info", f"BenchProd reached MEASUREMENT_S={MEASUREMENT_S} — broadcasting drain")
            api.band_broadcast(SHUTDOWN_BAND, {"cmd": "drain"})
            api.stop()
    return True


def on_stop(api) -> None:
    t_end = time.perf_counter()
    elapsed = max(t_end - _t0, 1e-9)
    m = api.metrics()
    loop = m.get("loop", {}) or {}
    role = m.get("role", {}) or {}
    iters = loop.get("iteration_count", 0)
    steady_slots = (_count - _warmup_count) if _warmup_count >= 0 else 0
    steady_s     = (t_end - _warmup_t)      if _warmup_count >= 0 else 0.0
    steady_rate  = (steady_slots / max(steady_s, 1e-9)) if steady_slots > 0 else 0.0
    api.log("info",
            f"BENCH-PROD engine={ENGINE} "
            f"total={_count} elapsed_s={elapsed:.3f} avg_rate={_count/elapsed:.0f} "
            f"steady_slots={steady_slots} steady_s={steady_s:.3f} steady_rate={steady_rate:.0f} "
            f"iters={iters} iters_per_s={iters/elapsed:.0f} "
            f"written={role.get('out_slots_written',0)} "
            f"drops={role.get('out_drop_count',0)} "
            f"work_us_last={loop.get('last_cycle_work_us',0)}")
