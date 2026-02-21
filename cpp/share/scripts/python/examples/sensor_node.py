"""
sensor_node.py — Multi-role actor example: simultaneous producer + consumer.

Demonstrates how a single actor can host two independent roles:
  - "raw_out"  (producer): publishes temperature measurements at 10 Hz
  - "cfg_in"   (consumer): receives setpoint commands from a controller

Each role runs in its own C++ thread.  Python callbacks are invoked with
the GIL held; module-level state is shared safely because only one role's
callback runs at a time.

Slot and FlexZone layouts are declared in sensor_node.json — no
struct.pack/unpack needed.  ctypes.LittleEndianStructure fields are
accessed by name (e.g. slot.value, slot.ts).

Object lifetime rules
---------------------
  slot     — Valid ONLY during on_write()/on_read().  Do not store it.
             Producer slot: writable ctypes view into SHM.
             Consumer slot: read-only ctypes view (field writes → TypeError).
  flexzone — Valid for the role's entire lifetime.  Safe to store.
  api      — Always valid.  Stateless proxy to C++ services.

Usage
-----
    pylabhub-actor --config sensor_node.json
    pylabhub-actor --config sensor_node.json --validate
    pylabhub-actor --config sensor_node.json --list-roles
"""

import math
import os
import time

import pylabhub_actor as actor

# ---------------------------------------------------------------------------
# Shared module-level state
# (C++ GIL serialises Python callbacks — no extra locking needed for these)
# ---------------------------------------------------------------------------
current_setpoint: float = 20.0   # updated by cfg_in role
measurement_count: int  = 0
setpoint_updates:  int  = 0


# ===========================================================================
# Role: raw_out (producer)
# ===========================================================================

@actor.on_init("raw_out")
def raw_out_init(flexzone, api: actor.ActorRoleAPI):
    """
    Called once after the producer SHM region is ready.

    Write device metadata into the flexzone (shared memory), then stamp its
    BLAKE2b checksum so consumers can verify integrity when they connect.
    """
    flexzone.device_id   = 42
    flexzone.sample_rate = 10       # 10 Hz
    flexzone.label       = b"temperature_sensor"
    api.update_flexzone_checksum()
    api.log('info',
            f"raw_out: started  uid={api.uid()}  device_id=42  "
            f"pid={os.getpid()}")


@actor.on_write("raw_out")
def raw_out_write(slot, flexzone, api: actor.ActorRoleAPI) -> bool:
    """
    Called every interval_ms (100 ms = 10 Hz).

    slot is a writable ctypes.LittleEndianStructure backed by the SHM slot.
    Fill fields and return True to commit (makes the slot visible to consumers).
    Return False to discard (slot is not published; no ZMQ broadcast).
    """
    global measurement_count, current_setpoint
    measurement_count += 1

    # Simulate a temperature reading with a sine wave around the setpoint.
    t            = time.time()
    slot.ts      = t
    slot.value   = current_setpoint + 0.5 * math.sin(2 * math.pi * t / 10.0)
    slot.flags   = 0x01             # bit 0: data valid
    # Fill the 8-sample array with recent readings (demo: same value repeated)
    for i in range(8):
        slot.samples[i] = slot.value

    if measurement_count % 100 == 0:        # log every 10 s
        api.log('info',
                f"raw_out: count={measurement_count}  "
                f"value={slot.value:.3f}  setpoint={current_setpoint:.2f}  "
                f"consumers={len(api.consumers())}")
    return True


@actor.on_message("raw_out")
def raw_out_message(sender: str, data: bytes, api: actor.ActorRoleAPI):
    """
    Called when a consumer sends a ZMQ ctrl frame to this producer.
    Useful for ctrl/command-response patterns without a separate control channel.
    """
    cmd = data.decode(errors='replace')
    api.log('debug', f"raw_out: ctrl from {sender}: '{cmd}'")
    if cmd == "ping":
        api.send(sender, b"pong")
    else:
        api.send(sender, b"ack")


@actor.on_stop("raw_out")
def raw_out_stop(flexzone, api: actor.ActorRoleAPI):
    """Called once after the write loop exits (on shutdown or api.stop())."""
    api.log('info',
            f"raw_out: stopped  total_measurements={measurement_count}")


# ===========================================================================
# Role: cfg_in (consumer)
# ===========================================================================

@actor.on_init("cfg_in")
def cfg_in_init(flexzone, api: actor.ActorRoleAPI):
    """
    Called once before the consumer read loop starts.

    The cfg_in role has no flexzone_schema in the config, so flexzone is None.
    If a flexzone_schema were declared, this would be a read-only zero-copy
    view of the producer's SHM flexzone.
    """
    api.log('info', f"cfg_in: connected  uid={api.uid()}")
    # Announce ourselves to the setpoint controller.
    api.send_ctrl(b"hello:sensor_node_001")


@actor.on_read("cfg_in")
def cfg_in_read(slot, flexzone, api: actor.ActorRoleAPI, *, timed_out: bool = False):
    """
    Called for each setpoint slot, or on timeout (timed_out=True).

    When timed_out=True: slot is None; no new setpoint arrived in timeout_ms.
    slot fields are read-only — writing raises TypeError.
    """
    global current_setpoint, setpoint_updates

    if timed_out:
        # No setpoint update in 5 s — send a heartbeat to keep connection alive.
        api.send_ctrl(b"heartbeat")
        api.log('debug', "cfg_in: timeout — heartbeat sent")
        return

    if not api.slot_valid():
        api.log('warn', "cfg_in: setpoint slot checksum failed — ignoring")
        return

    new_setpoint   = slot.setpoint
    mode           = slot.mode
    sequence_id    = slot.sequence_id
    setpoint_updates += 1

    # Update the shared setpoint (read by raw_out_write above).
    current_setpoint = new_setpoint

    api.log('info',
            f"cfg_in: setpoint={new_setpoint:.2f}  mode={mode}  "
            f"seq={sequence_id}  total_updates={setpoint_updates}")


@actor.on_data("cfg_in")
def cfg_in_data(data: bytes, api: actor.ActorRoleAPI):
    """
    Called for each ZMQ broadcast frame (non-slot messages from the controller).
    """
    api.log('debug', f"cfg_in: zmq broadcast {len(data)} bytes")


@actor.on_stop_c("cfg_in")
def cfg_in_stop(flexzone, api: actor.ActorRoleAPI):
    """Called once after the consumer read loop exits."""
    api.log('info',
            f"cfg_in: stopped  setpoint_updates={setpoint_updates}")
