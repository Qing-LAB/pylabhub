"""
Processor: DemoDoubler

Reads each slot from 'lab.demo.counter', doubles `value`, and writes
'lab.demo.processed'. Also updates output flexzone metadata.
"""

import time

import pylabhub_processor as proc

_processed: int = 0


def on_init(api: proc.ProcessorAPI) -> None:
    fz = api.flexzone()
    if fz is not None:
        fz.stage = b"processor"
        fz.label = b"lab.demo.processed"
        fz.last_count = 0
        fz.last_value = 0.0
        fz.updated_ts = time.time()
        api.update_flexzone_checksum()

    api.log("info", f"DemoDoubler: started uid={api.uid()} {api.in_channel()} -> {api.out_channel()}")


def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
    global _processed

    if in_slot is None:
        api.log("debug", "DemoDoubler: timeout - no slot from producer")
        return False

    for sender, data in messages:
        api.log("debug", f"DemoDoubler: zmq from {sender}: {data!r}")

    out_slot.count = in_slot.count
    out_slot.ts = in_slot.ts
    out_slot.value = in_slot.value
    out_slot.doubled = in_slot.value * 2.0

    if flexzone is not None:
        flexzone.last_count = out_slot.count
        flexzone.last_value = out_slot.doubled
        flexzone.updated_ts = out_slot.ts
        api.update_flexzone_checksum()

    _processed += 1
    if _processed % 100 == 0:
        api.log(
            "info",
            f"DemoDoubler: processed={_processed} in={api.in_slots_received()} "
            f"out={api.out_slots_written()} drops={api.out_drop_count()}",
        )

    return True


def on_stop(api: proc.ProcessorAPI) -> None:
    api.log(
        "info",
        f"DemoDoubler: stopped processed={_processed} in={api.in_slots_received()} "
        f"out={api.out_slots_written()} drops={api.out_drop_count()}",
    )
