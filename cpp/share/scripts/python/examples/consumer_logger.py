"""
consumer_logger.py — Example pylabhub-actor consumer script (multi-role API).

Subscribes to the counter channel via role "counter_in" and logs each slot.
The slot layout is declared in consumer_logger.json — no struct.unpack() needed.

Slot fields  (ctypes.LittleEndianStructure, natural alignment):
  count  int64    — monotonic counter from producer
  ts     float64  — producer Unix timestamp

FlexZone fields (read-only zero-copy view of producer's SHM region):
  producer_pid  uint64    — producer PID
  start_time    float64   — producer start time
  label         c_char*32 — channel label

Usage
-----
    pylabhub-actor --config consumer_logger.json
    (Requires producer_counter running on the same broker.)

Notes
-----
- The role name "counter_in" in @actor.on_read() must match the key in the
  "roles" map in consumer_logger.json.
- `slot` is a ctypes.from_buffer view into read-only SHM memory.
  Writing to slot fields raises TypeError.  Do not store `slot` beyond the call.
- `flexzone` is a read-only zero-copy view of the producer's flexzone region.
  It updates automatically as the producer writes to it.
- `timed_out=True` fires when no slot arrived within timeout_ms (5 s here).
"""

import time

import pylabhub_actor as actor

# ---------------------------------------------------------------------------
# Module-level state
# ---------------------------------------------------------------------------
last_count = 0
slots_read = 0
start_time = 0.0


# ---------------------------------------------------------------------------
# Role: counter_in (consumer)
# ---------------------------------------------------------------------------

@actor.on_init("counter_in")
def counter_in_init(flexzone, api: actor.ActorRoleAPI):
    """
    Called once before the read loop starts.

    flexzone is a read-only ctypes.LittleEndianStructure backed directly by
    the producer's SHM flexible-zone region (zero-copy).
    """
    global start_time
    start_time = time.time()
    pid   = flexzone.producer_pid
    label = flexzone.label.decode(errors='replace') if flexzone.label else ''
    fz_ok = api.verify_flexzone_checksum()
    api.log('info',
            f"counter_in: connected  uid={api.uid()}  "
            f"producer_pid={pid}  label='{label}'  flexzone_valid={fz_ok}")


@actor.on_read("counter_in")
def counter_in_read(slot, flexzone, api: actor.ActorRoleAPI, *, timed_out: bool = False):
    """
    Called for each SHM slot or on timeout.

    When timed_out=True: slot is None; no data is available.
    When timed_out=False: slot is a read-only ctypes struct — valid this call only.
    api.slot_valid() is False when checksum failed and on_checksum_fail='pass'.
    """
    global last_count, slots_read

    if timed_out:
        # No data in 5 s — send a heartbeat to the producer.
        api.send_ctrl(b"heartbeat")
        api.log('debug', "counter_in: timeout heartbeat sent")
        return

    if not api.slot_valid():
        api.log('warn', "counter_in: slot checksum failed — data may be corrupt")

    count = slot.count
    ts    = slot.ts
    slots_read += 1

    skipped = count - last_count - 1
    last_count = count

    if skipped > 0:
        api.log('warn', f"counter_in: skipped {skipped} slot(s) at count={count}")

    if count % 1000 == 0:
        elapsed = time.time() - start_time
        rate = slots_read / elapsed if elapsed > 0 else 0.0
        api.log('info',
                f"counter_in: slot {count}  ts={ts:.3f}  "
                f"rate={rate:.0f} slots/s")


@actor.on_data("counter_in")
def counter_in_data(data: bytes, api: actor.ActorRoleAPI):
    """Called for each ZMQ broadcast frame (non-slot messages from producer)."""
    api.log('debug', f"counter_in: zmq frame {len(data)} bytes")


@actor.on_stop_c("counter_in")
def counter_in_stop(flexzone, api: actor.ActorRoleAPI):
    """Called once after the read loop exits."""
    elapsed = time.time() - start_time
    api.log('info',
            f"counter_in: stopped  read={slots_read} slots  "
            f"elapsed={elapsed:.1f}s")
