"""
Bridge A: pass-through — copies SHM input to ZMQ output unchanged.
Stops when upstream channel_closing arrives; this closes the ZMQ output,
triggering channel_closing in Bridge B.

Pipeline: Producer [SHM] -> Bridge A [ZMQ PUSH] -> Bridge B [ZMQ PULL] -> [SHM] -> Consumer
"""

import pylabhub_processor as proc


def on_init(api: proc.ProcessorAPI) -> None:
    api.log('info',
            f"BridgeA: started  {api.in_channel()} -> {api.out_channel()}"
            f"  (SHM -> ZMQ PUSH)")


def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
    # Propagate shutdown downstream.
    for msg in messages:
        if isinstance(msg, dict) and msg.get("event") == "channel_closing":
            api.log('info', "BridgeA: upstream channel closed — stopping")
            api.stop()
            return False

    if in_slot is None:
        return False

    out_slot.count = in_slot.count
    out_slot.ts    = in_slot.ts
    out_slot.value = in_slot.value
    return True


def on_stop(api: proc.ProcessorAPI) -> None:
    api.log('info',
            f"BridgeA: stopped  in={api.in_slots_received()}"
            f"  out={api.out_slots_written()}")
