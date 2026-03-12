# pylabhub Pipeline Demo

End-to-end demo showing hub + producer + processor + consumer connected
through shared memory and ZMQ.

## Pipeline

```
DemoProducer  ──▶  lab.demo.counter  ──▶  DemoDoubler  ──▶  lab.demo.processed  ──▶  DemoConsumer
(producer)          (SHM + ZMQ)         (processor)          (SHM + ZMQ)            (consumer)
```

The processor reads each incoming slot, doubles the 1024-sample payload, appends
`scale=2.0`, and publishes the result on the output channel.

## Throughput profile (current demo mode)

The demo is configured as a throughput benchmark:

- Producer writes `count + ts + samples[1024]` (`float32`) on `lab.demo.counter` at **max rate** (`target_period_ms=0, loop_timing="max_rate"`)
- Processor doubles `samples[]`, sets `scale=2.0`, and writes `lab.demo.processed`
- Consumer prints throughput once per second:
  - slots/sec
  - MiB/sec (`slot_size * slots / interval`)
  - overflow counters inferred from `count` sequence gaps
  - `data=OK/MISMATCH` correctness check (validates `samples[0]` and `samples[-1]` against expected values, and `scale==2.0`)

## Quick start

```bash
# From cpp/ — build first if needed:
cmake --build build

# Run the demo (all four processes, Ctrl-C to stop):
bash share/demo/run_demo.sh

# If your current directory is the repository root:
bash cpp/share/demo/run_demo.sh
```

## Directory layout

```
share/demo/
├── hub/
│   ├── hub.json                          # Hub config used by run_demo.sh (with --dev)
│   └── schemas/                          # Named schema library root (<hub_dir>/schemas)
│       └── lab/demo/
│           ├── counter.v1.json           # lab.demo.counter@1
│           └── processed.v1.json         # lab.demo.processed@1
├── producer/
│   ├── producer.json                     # Producer config: channel, schema, shm
│   └── script/
│       └── python/
│           └── __init__.py               # ← edit this to change producer logic
├── processor/
│   ├── processor.json                    # Processor config: in/out channels, schemas
│   └── script/
│       └── python/
│           └── __init__.py               # ← edit this to change transform logic
└── consumer/
    ├── consumer.json                     # Consumer config: channel, schema, shm
    └── script/
        └── python/
            └── __init__.py               # ← edit this to change consumer logic
```

## Swapping scripts

Scripts are isolated Python packages.  To replace the transform logic:

1. Edit `processor/script/python/__init__.py` — change `on_process()`.
2. Re-run `bash share/demo/run_demo.sh`.  No recompile needed.

The same applies to the producer and consumer scripts.

## Script interface summary

### Producer (`pylabhub_producer`)

```python
import pylabhub_producer as prod

def on_init(api: prod.ProducerAPI) -> None:
    """Called once before the loop."""

def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:
    """
    Called every iteration.
      out_slot — writable ctypes struct, or None on timeout
      flexzone — persistent flexzone struct, or None
      messages — list of items; each item is either a (sender: str, data: bytes) tuple
                 (inbox data) or a dict {"event": str, ...} (lifecycle event)
      api      — ProducerAPI proxy
    Return True/None to commit; False to discard.
    """

def on_stop(api: prod.ProducerAPI) -> None:
    """Called once on shutdown."""
```

### Consumer (`pylabhub_consumer`)

```python
import pylabhub_consumer as cons

def on_init(api: cons.ConsumerAPI) -> None:
    """Called once before the loop."""

def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:
    """
    Called for each received slot, or on timeout.
      in_slot  — zero-copy ctypes struct (write-guarded via __setattr__), or None on timeout
      flexzone — zero-copy writable ctypes struct (user-coordinated R/W), or None
      messages — list of items; each item is either a (sender: str, data: bytes) tuple
                 (inbox data) or a dict {"event": str, ...} (lifecycle event)
      api      — ConsumerAPI proxy
    No return value.
    """

def on_stop(api: cons.ConsumerAPI) -> None:
    """Called once on shutdown."""
```

### Processor (`pylabhub_processor`)

```python
import pylabhub_processor as proc

def on_init(api: proc.ProcessorAPI) -> None:
    """Called once before the processing loop."""

def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
    """
    Called for each input slot.
      in_slot  — zero-copy ctypes struct from in_channel (write-guarded), or None on timeout
      out_slot — writable ctypes struct for out_channel, or None on timeout
      flexzone — output-side persistent flexzone, or None
      messages — list of items; each item is either a (sender: str, data: bytes) tuple
                 (inbox data) or a dict {"event": str, ...} (lifecycle event)
      api      — ProcessorAPI proxy
    Return True/None to commit; False to discard.
    """

def on_stop(api: proc.ProcessorAPI) -> None:
    """Called once on shutdown."""
```

## Schema matching rules

| Component | JSON key | Meaning |
|-----------|----------|---------|
| Producer | `slot_schema` | Shape of the SHM data slot written each iteration |
| Consumer | `slot_schema` | Must match the producer's `slot_schema` exactly |
| Processor in | `in_slot_schema` | Must match the upstream producer's `slot_schema` |
| Processor out | `out_slot_schema` | Shape of the output slot; consumer's `slot_schema` must match |
| All stages | `flexzone_schema` | Must match along the connected path when flexzone is enabled |

Field types: `int8/16/32/64`, `uint8/16/32/64`, `float32`, `float64`, `string`/`char` (fixed-length), plus `"count": N` for arrays.

## Named schema directory setup

This demo uses named schema IDs in all actor configs:

- `lab.demo.counter@1`
- `lab.demo.processed@1`

They are resolved from `<hub_dir>/schemas`, so these files must exist:

```text
share/demo/hub/schemas/lab/demo/counter.v1.json
share/demo/hub/schemas/lab/demo/processed.v1.json
```

## Slot types and field access

Slot objects (`in_slot`, `out_slot`) are **zero-copy views** into shared memory.

### Scalar fields

```python
out_slot.ts    = time.monotonic()   # float64 — writes directly into SHM
out_slot.value = sensor_reading     # int32
```

Consumer `in_slot` is write-guarded: assigning a field raises `AttributeError`.

### Array fields (`"count": N > 1`)

Fields with `"count": N` become ctypes arrays. Use `np.ctypeslib.as_array()` for numpy access:

```python
import numpy as np

# Read-side (consumer or processor in_slot):
arr = np.ctypeslib.as_array(in_slot.samples)    # shape=(N,), dtype matches field type

# Write-side (producer out_slot or processor out_slot):
arr = np.ctypeslib.as_array(out_slot.samples)
arr[:] = new_data                                # writes directly into SHM slot
```

`expose_as` is slot-level — all fields in a ctypes slot remain ctypes types.
Manual `np.ctypeslib.as_array()` is the correct way to get per-field numpy arrays.

### Raw buffer access

```python
data = bytes(in_slot)                # immutable copy (all bytes)
view = memoryview(in_slot).cast('B') # zero-copy byte view
```

---

## hub_dir integration

For production use, replace the inline `"broker"` field in each config with
a `hub_dir` key pointing to a shared hub directory:

```json
{
  "hub_dir": "/path/to/my_hub",
  ...
}
```

`pylabhub-hubshell my_hub/` writes `hub.json` (broker endpoint) and
`hub.pubkey` (CurveZMQ public key) to that directory.  All producers,
consumers, and processors automatically pick up the endpoint and use
encrypted transport.
