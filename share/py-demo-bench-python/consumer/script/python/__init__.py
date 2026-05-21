"""Engine-throughput bench: Python consumer.

Drains producer's ring.  Joins band lazily; on_band_message triggers
api.stop() on drain.  Warmup-snapshots received count for steady-rate
report.
"""

import time

ENGINE        = "python"
SHUTDOWN_BAND = "!bench.shutdown"
WARMUP_S      = 5.0

_received = 0
_t0 = 0.0
_warmup_received = -1
_warmup_t = 0.0
_band_joined = False


def on_init(api) -> None:
    global _t0
    _t0 = time.perf_counter()
    api.log("info", "BenchCons-Python started")


def on_consume(rx, messages, api) -> bool:
    global _received, _warmup_received, _warmup_t, _band_joined
    if not _band_joined:
        res = api.band_join(SHUTDOWN_BAND)
        _band_joined = True
        if res is not None and res.get("status") == "success":
            api.log("info", f"BenchCons joined band '{SHUTDOWN_BAND}'")
    if rx.slot is None:
        return True
    _received += 1
    if _warmup_received < 0 and (_received & 0x3FF) == 0:
        elapsed = time.perf_counter() - _t0
        if elapsed >= WARMUP_S:
            _warmup_received = _received
            _warmup_t = time.perf_counter()
    return True


def on_band_message(band, sender, body, api) -> None:
    if band == SHUTDOWN_BAND and isinstance(body, dict) and body.get("cmd") == "drain":
        api.log("info", f"BenchCons received drain from {sender} — stopping")
        api.stop()


def on_stop(api) -> None:
    t_end = time.perf_counter()
    elapsed = max(t_end - _t0, 1e-9)
    m = api.metrics()
    loop = m.get("loop", {}) or {}
    role = m.get("role", {}) or {}
    iters = loop.get("iteration_count", 0)
    steady_slots = (_received - _warmup_received) if _warmup_received >= 0 else 0
    steady_s     = (t_end - _warmup_t)            if _warmup_received >= 0 else 0.0
    steady_rate  = (steady_slots / max(steady_s, 1e-9)) if steady_slots > 0 else 0.0
    api.log("info",
            f"BENCH-CONS engine={ENGINE} "
            f"total={_received} elapsed_s={elapsed:.3f} avg_rate={_received/elapsed:.0f} "
            f"steady_slots={steady_slots} steady_s={steady_s:.3f} steady_rate={steady_rate:.0f} "
            f"iters={iters} iters_per_s={iters/elapsed:.0f} "
            f"in_received={role.get('in_slots_received',0)} "
            f"work_us_last={loop.get('last_cycle_work_us',0)}")
