"""
sensor_node.py — Example pylabhub-producer script for a temperature sensor.

Publishes temperature measurements at 10 Hz on 'lab.sensor.temperature'.
The slot layout is declared in sensor_node.json — no struct.pack() needed.

Slot fields:
  ts       float64      — Unix timestamp (seconds)
  value    float32      — temperature reading (°C, simulated)
  flags    uint8        — status flags (0 = nominal)
  samples  float32[8]   — last 8 raw ADC samples

FlexZone fields (written once, readable by all connected consumers):
  device_id    uint16    — hardware device identifier
  sample_rate  uint32    — ADC sample rate in Hz
  label        c_char*32 — channel label string

Usage
-----
    # Place this file at <prod_dir>/script/python/__init__.py, then:
    pylabhub-producer <prod_dir>

    # Or point directly at the config:
    pylabhub-producer --config sensor_node.json

Notes
-----
- This example replaces the old multi-role actor sensor_node.  In the new
  architecture, the producer and consumer roles are separate processes:
    - sensor_node.json / sensor_node.py  → standalone pylabhub-producer
    - consumer_logger.json / consumer_logger.py → standalone pylabhub-consumer
- `out_slot` is valid ONLY during on_produce(). Do not store it.
- `flexzone` is persistent and safe to store between calls.
"""

import math
import os
import time

import pylabhub_producer as prod

# ---------------------------------------------------------------------------
# Module-level state
# ---------------------------------------------------------------------------
_count:      int   = 0
_start_time: float = 0.0
_device_id:  int   = 0x0042   # simulated hardware ID


def on_init(api: prod.ProducerAPI) -> None:
    """Called once after SHM is ready, before the write loop starts."""
    global _start_time
    _start_time = time.time()

    fz = api.flexzone()
    if fz is not None:
        fz.device_id   = _device_id
        fz.sample_rate = 100         # 100 Hz simulated ADC
        fz.label       = b"lab.sensor.temperature"
        api.update_flexzone_checksum()

    api.log('info',
            f"TemperatureSensor: started  uid={api.uid()}"
            f"  device_id=0x{_device_id:04X}  pid={os.getpid()}")


def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:
    """
    Called every 100 ms to produce one temperature measurement.

    Simulates a temperature reading as:  20 + sin(t * 0.5) * 5  °C
    Populates samples[] with 8 Gaussian-noise ADC readings around the value.
    """
    global _count
    if out_slot is None:
        return False  # SHM backpressure

    _count += 1
    t    = time.time()
    temp = 20.0 + math.sin(t * 0.5) * 5.0   # °C, slow sinusoidal variation

    out_slot.ts    = t
    out_slot.value = temp
    out_slot.flags = 0  # nominal

    # Simulate 8 ADC samples around the temperature value
    import random
    for i in range(8):
        out_slot.samples[i] = temp + random.gauss(0.0, 0.05)

    for sender, data in messages:
        api.log('debug', f"TemperatureSensor: ctrl from {sender}: {data!r}")

    if _count % 100 == 0:  # log every 10 s
        api.log('info',
                f"TemperatureSensor: count={_count}"
                f"  temp={temp:.2f} C"
                f"  consumers={len(api.consumers())}")
    return True


def on_stop(api: prod.ProducerAPI) -> None:
    """Called once after the write loop exits."""
    elapsed = time.time() - _start_time
    api.log('info',
            f"TemperatureSensor: stopped  count={_count}  elapsed={elapsed:.1f}s")
