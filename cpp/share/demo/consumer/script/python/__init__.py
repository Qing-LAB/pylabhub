"""
Consumer: DemoConsumer

Reads each slot from 'lab.demo.processed' and logs values plus flexzone.
"""

import time

import pylabhub_consumer as cons

_received: int = 0
_last_ts: float = 0.0
_verify_ok: int = 0
_verify_fail: int = 0
_verify_pending: int = 0
_recent_slots: dict[int, float] = {}
_recent_limit: int = 256


def _text(value) -> str:
    if isinstance(value, bytes):
        return value.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
    return str(value)


def on_init(api: cons.ConsumerAPI) -> None:
    api.log("info", f"DemoConsumer: started uid={api.uid()}")


def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:
    global _received, _last_ts, _verify_ok, _verify_fail, _verify_pending

    if in_slot is None:
        api.log("debug", "DemoConsumer: timeout - no slot from processor")
        return

    _received += 1
    now = time.time()
    rate = 1.0 / (now - _last_ts) if _last_ts > 0 else 0.0
    _last_ts = now

    # Keep a bounded cache of recently consumed output slots so we can verify
    # flexzone data when its count overlaps with observed slot history.
    slot_count = int(in_slot.count)
    slot_doubled = float(in_slot.doubled)
    _recent_slots[slot_count] = slot_doubled
    if len(_recent_slots) > _recent_limit:
        oldest = min(_recent_slots.keys())
        del _recent_slots[oldest]

    fz_stage = ""
    fz_label = ""
    fz_count = None
    fz_value = None
    fz_verify = "N/A"
    if flexzone is not None:
        fz_stage = _text(getattr(flexzone, "stage", ""))
        fz_label = _text(getattr(flexzone, "label", ""))
        fz_count = int(getattr(flexzone, "last_count", -1))
        fz_value = float(getattr(flexzone, "last_value", 0.0))
        # Race-tolerant verification:
        # Flexzone is channel-shared and may lead/lag the slot currently consumed.
        # Verify only when flexzone count is in our observed slot history.
        expected = _recent_slots.get(fz_count)
        if expected is None:
            _verify_pending += 1
            fz_verify = "PENDING"
        elif abs(fz_value - expected) < 1e-4:
            _verify_ok += 1
            fz_verify = "OK"
        else:
            _verify_fail += 1
            fz_verify = "FAIL"
            api.log(
                "error",
                f"Flexzone verify failed: fz(last_count={fz_count}, last_value={fz_value:.6f}) "
                f"vs observed_slot_doubled={expected:.6f}",
            )

    if _received % 50 == 0:
        api.log(
            "info",
            f"DemoConsumer: count={in_slot.count} value={in_slot.value:.3f} "
            f"doubled={in_slot.doubled:.3f} rate={rate:.1f} Hz total={_received} "
            f"fz.stage={fz_stage} fz.label={fz_label} verify={fz_verify} "
            f"ok={_verify_ok} fail={_verify_fail} pending={_verify_pending}",
        )
    else:
        print(
            f"  count={in_slot.count:6d} ts={in_slot.ts:.3f} "
            f"value={in_slot.value:8.3f} doubled={in_slot.doubled:8.3f} "
            f"rate={rate:5.1f} Hz fz[{fz_stage}:{fz_label}] "
            f"verify={fz_verify} ok={_verify_ok} fail={_verify_fail} pending={_verify_pending}",
            flush=True,
        )


def on_stop(api: cons.ConsumerAPI) -> None:
    api.log(
        "info",
        f"DemoConsumer: stopped total_received={_received} "
        f"verify_ok={_verify_ok} verify_fail={_verify_fail} verify_pending={_verify_pending}",
    )
