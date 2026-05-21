"""Throughput producer with band-coordinated shutdown.

Writes count + ts + samples(4096 float32) per slot at max_rate.

Coordinated shutdown via HEP-CORE-0030 band:
- on_init joins `!demo.shutdown`.
- on_band_message, when the body is the bytes `b"drain"`, calls
  api.stop() so this role exits cleanly without relying on the
  harness's SIGTERM cascade.  Closes Audit B9 (teardown race
  between SIGTERM'd hub and producer's BRC ctrl thread).
"""

import math
import time

import numpy as np

BLOCK_SIZE = 4096
PHASE_STEP = 2.0 * math.pi / 256.0
SHUTDOWN_BAND = "!demo.shutdown"

_count = 0
_t0 = 0.0
_base = np.arange(BLOCK_SIZE, dtype=np.float32) * np.float32(PHASE_STEP)


_band_joined = False


def on_init(api) -> None:
    global _t0
    _t0 = time.time()
    # Cannot call api.band_join here — on_init runs BEFORE
    # start_handler_threads (HEP-CORE-0011 §"Initialization Protocol"
    # Step 5 vs Step 6), so the BRC handler isn't up yet and
    # band_join silently fails.  Lazy-init in on_produce instead.
    api.log("info",
            f"DemoProducer started uid={api.uid()} channel={api.channel()} "
            f"block={BLOCK_SIZE} float32 max_rate band={SHUTDOWN_BAND}")


def on_produce(tx, messages, api) -> bool:
    global _count, _band_joined
    if not _band_joined:
        res = api.band_join(SHUTDOWN_BAND)
        if res is not None and res.get("status") == "success":
            _band_joined = True
            api.log("info", f"DemoProducer joined band '{SHUTDOWN_BAND}'")
        else:
            api.log("warn", f"DemoProducer band_join('{SHUTDOWN_BAND}') failed: {res}")
            _band_joined = True  # don't keep retrying
    if tx.slot is None:
        return False
    _count += 1
    tx.slot.count = _count
    tx.slot.ts    = time.time()
    arr = api.as_numpy(tx.slot.samples)
    arr[:] = np.sin(_base + np.float32(_count * 0.01))
    if _count % 5000 == 0:
        elapsed = time.time() - _t0
        api.log("info",
                f"DemoProducer wrote slot count={_count} "
                f"rate={_count/max(elapsed, 1e-9):.0f} slots/s "
                f"throughput={_count*BLOCK_SIZE*4/(1024*1024)/max(elapsed,1e-9):.1f} MiB/s")
    return True


def on_band_message(band, sender, body, api) -> None:
    if band == SHUTDOWN_BAND and isinstance(body, dict) and body.get("cmd") == "drain":
        api.log("info",
                f"DemoProducer received 'drain' on '{band}' from "
                f"'{sender}' — stopping cleanly")
        api.stop()


def on_stop(api) -> None:
    elapsed = max(time.time() - _t0, 1e-9)
    m = api.metrics()
    loop = m.get("loop", {}) or {}
    q    = m.get("queue", {}) or {}
    role = m.get("role", {}) or {}
    iters = loop.get("iteration_count", 0)
    api.log("info",
            f"DemoProducer STOPPED total={_count} "
            f"avg_rate={_count/elapsed:.0f} slots/s "
            f"avg_throughput={_count*BLOCK_SIZE*4/(1024*1024)/elapsed:.1f} MiB/s "
            f"| iters={iters} ({iters/elapsed:.0f}/s) "
            f"work_us_last={loop.get('last_cycle_work_us','n/a')} "
            f"overruns={loop.get('loop_overrun_count','n/a')} "
            f"acquire_retries={loop.get('acquire_retry_count','n/a')} "
            f"slot_wait_us_last={q.get('last_slot_wait_us','n/a')} "
            f"written={role.get('out_slots_written','n/a')} "
            f"drops={role.get('out_drop_count','n/a')}")
