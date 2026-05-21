"""Throughput consumer with band-coordinated shutdown.

Reads processor's processed slot + flexzone running stats; once
SLOT_TARGET messages have been received, broadcasts `b"drain"` to
the `!demo.shutdown` band so the producer and processor can exit
cleanly on their own, then calls api.stop() to exit itself.

This avoids the harness's SIGTERM cascade and the resulting
`Socket operation on non-socket` teardown race (Audit B9).  All
roles exit via api.stop() under their own steam; the hub stays
alive until they've deregistered.
"""

import time

BLOCK_SIZE = 4096
SLOT_BYTES_IN = 8 + 8 + 4 * BLOCK_SIZE + 8 * 4   # 16432 per slot
SHUTDOWN_BAND  = "!demo.shutdown"
SLOT_TARGET    = 20000  # ~5 s at ~4 kHz; sufficient for stable rate

_received = 0
_t0 = 0.0
_fz = None
_drain_sent = False


_band_joined = False


def on_init(api) -> None:
    global _t0, _fz
    _t0 = time.time()
    _fz = api.flexzone()
    # band_join deferred to on_consume (handler not yet up here —
    # see HEP-CORE-0011 §"Initialization Protocol").
    api.log("info",
            f"DemoConsumer started uid={api.uid()} channel={api.channel()} "
            f"slot_bytes={SLOT_BYTES_IN} target={SLOT_TARGET} "
            f"band={SHUTDOWN_BAND}")


def on_consume(rx, messages, api) -> bool:
    global _received, _drain_sent, _band_joined
    if not _band_joined:
        res = api.band_join(SHUTDOWN_BAND)
        if res is not None and res.get("status") == "success":
            _band_joined = True
            api.log("info", f"DemoConsumer joined band '{SHUTDOWN_BAND}'")
        else:
            api.log("warn", f"DemoConsumer band_join('{SHUTDOWN_BAND}') failed: {res}")
            _band_joined = True
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
    # Trigger coordinated shutdown once we've collected enough.
    if not _drain_sent and _received >= SLOT_TARGET:
        _drain_sent = True
        api.log("info",
                f"DemoConsumer reached target={SLOT_TARGET} — "
                f"broadcasting 'drain' on '{SHUTDOWN_BAND}' and stopping")
        api.band_broadcast(SHUTDOWN_BAND, {"cmd": "drain"})
        api.stop()
    return True


def on_band_message(band, sender, body, api) -> None:
    # Consumer subscribes to its own band so this fires; we
    # already initiated stop above, so this is a no-op log only.
    if band == SHUTDOWN_BAND and isinstance(body, dict) and body.get("cmd") == "drain":
        api.log("info",
                f"DemoConsumer saw own drain echo on '{band}' "
                f"from '{sender}' (no-op)")


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
