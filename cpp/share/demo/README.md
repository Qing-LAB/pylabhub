# pylabhub Pipeline Demo

End-to-end demo showing hub + producer + processor + consumer connected
through shared memory and ZMQ.

## Pipeline

```
DemoProducer  ──▶  lab.demo.counter  ──▶  DemoDoubler  ──▶  lab.demo.processed  ──▶  DemoConsumer
(producer)          (SHM + ZMQ)         (processor)          (SHM + ZMQ)            (consumer)
```

The processor reads each incoming slot, doubles `value`, and publishes the
enriched slot on the output channel.

## Quick start

```bash
# From the repo root — build first if needed:
cmake --build build

# Run the demo (all four processes, Ctrl-C to stop):
bash share/demo/run_demo.sh
```

## Directory layout

```
share/demo/
├── hub/
│   └── hub.json                          # Hub config (reference; --dev mode skips it)
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
      messages — list of (sender: str, data: bytes)
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
      in_slot  — read-only ctypes struct, or None on timeout
      flexzone — read-only flexzone struct, or None
      messages — list of (sender: str, data: bytes)
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
      in_slot  — read-only ctypes struct from in_channel, or None on timeout
      out_slot — writable ctypes struct for out_channel, or None on timeout
      flexzone — output-side persistent flexzone, or None
      messages — list of (sender: str, data: bytes)
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

Field types: `int8/16/32/64`, `uint8/16/32/64`, `float32`, `float64`, `string`/`char` (fixed-length), plus `"count": N` for arrays.

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
