"""
Bridge A: pass-through — copies SHM input to ZMQ output unchanged.

Pipeline: Producer [SHM] -> Bridge A [ZMQ PUSH] -> Bridge B [ZMQ PULL] -> [SHM] -> Consumer
"""

import pylabhub_processor as proc


def on_init(api: proc.ProcessorAPI) -> None:
    api.log('info',
            f"BridgeA: started  {api.in_channel()} -> {api.out_channel()}"
            f"  (SHM -> ZMQ PUSH)")


def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
    if in_slot is None:
        api.log('debug', "BridgeA: timeout")
        return False

    # Pass-through: copy all fields unchanged.
    out_slot.count = in_slot.count
    out_slot.ts    = in_slot.ts
    out_slot.value = in_slot.value
    return True


def on_stop(api: proc.ProcessorAPI) -> None:
    api.log('info',
            f"BridgeA: stopped  in={api.in_slots_received()}"
            f"  out={api.out_slots_written()}")
