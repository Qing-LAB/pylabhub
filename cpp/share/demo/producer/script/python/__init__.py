"""
Producer: DemoProducer

Publishes a monotonic counter, Unix timestamp, and sine-wave value
on channel 'lab.demo.counter'.

FlexZone layout:
  stage       c_char*16
  label       c_char*32
  last_count  int64
  last_value  float32
  updated_ts  float64
"""

import math
import time

import pylabhub_producer as prod

_count: int = 0
_start: float = 0.0


def on_init(api: prod.ProducerAPI) -> None:
    global _start
    _start = time.time()

    fz = api.flexzone()
    if fz is not None:
        fz.stage = b"producer"
        fz.label = b"lab.demo.counter"
        fz.last_count = 0
        fz.last_value = 0.0
        fz.updated_ts = _start
        api.update_flexzone_checksum()

    api.log("info", f"DemoProducer: started uid={api.uid()}")


def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:
    global _count

    if out_slot is None:
        return False

    _count += 1
    out_slot.count = _count
    out_slot.ts = time.time()
    out_slot.value = math.sin(_count * 0.1) * 10.0

    if flexzone is not None:
        flexzone.last_count = out_slot.count
        flexzone.last_value = out_slot.value
        flexzone.updated_ts = out_slot.ts
        api.update_flexzone_checksum()

    try:
        for sender, data in messages:
            api.log("debug", f"DemoProducer: ctrl from {sender}: {data!r}")
            api.send(sender, b"ack")
    except UnicodeDecodeError:
        api.log("debug", "DemoProducer: ignored non-UTF8 control message identity")

    if _count % 100 == 0:
        api.log(
            "info",
            f"DemoProducer: count={_count} value={out_slot.value:.3f} "
            f"consumers={len(api.consumers())}",
        )
    return True


def on_stop(api: prod.ProducerAPI) -> None:
    elapsed = time.time() - _start
    api.log("info", f"DemoProducer: stopped total={_count} elapsed={elapsed:.1f}s")
