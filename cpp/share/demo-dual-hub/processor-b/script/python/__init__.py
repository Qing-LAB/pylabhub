"""
Bridge B: transform — reads ZMQ input, doubles value, writes SHM output.
Stops when upstream channel_closing arrives; this closes the SHM output,
triggering channel_closing in the Consumer.

Pipeline: Producer [SHM] -> Bridge A [ZMQ PUSH] -> Bridge B [ZMQ PULL] -> [SHM] -> Consumer
"""

import pylabhub_processor as proc


def on_init(api: proc.ProcessorAPI) -> None:
    api.log('info',
            f"BridgeB: started  {api.in_channel()} -> {api.out_channel()}"
            f"  (ZMQ PULL -> SHM)")


def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
    # Propagate shutdown downstream.
    for msg in messages:
        if isinstance(msg, dict) and msg.get("event") == "channel_closing":
            api.log('info', "BridgeB: upstream channel closed — stopping")
            api.stop()
            return False

    if in_slot is None:
        return False

    out_slot.count   = in_slot.count
    out_slot.ts      = in_slot.ts
    out_slot.value   = in_slot.value
    out_slot.doubled = in_slot.value * 2.0
    return True


def on_stop(api: proc.ProcessorAPI) -> None:
    api.log('info',
            f"BridgeB: stopped  in={api.in_slots_received()}"
            f"  out={api.out_slots_written()}")
