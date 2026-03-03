# pylabhub Pipeline Demo

End-to-end demo showing hub + producer + processor + consumer connected
through shared memory and ZMQ.

## Pipeline

```
DemoProducer  ──▶  lab.demo.counter  ──▶  DemoDoubler  ──▶  lab.demo.processed  ──▶  DemoConsumer
(actor)              (SHM + ZMQ)         (processor)          (SHM + ZMQ)            (actor)
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
│   ├── actor.json                        # Producer config: channel, schema, shm
│   └── roles/
│       └── counter_out/
│           └── script/
│               └── __init__.py           # ← edit this to change producer logic
├── processor/
│   ├── processor.json                    # Processor config: in/out channels, schemas
│   └── script/
│       └── __init__.py                   # ← edit this to change transform logic
└── consumer/
    ├── actor.json                        # Consumer config: channel, schema, shm
    └── roles/
        └── counter_in/
            └── script/
                └── __init__.py           # ← edit this to change consumer logic
```

## Swapping scripts

Scripts are isolated Python packages.  To replace the transform logic:

1. Edit `processor/script/__init__.py` — change `on_process()`.
2. Re-run `bash share/demo/run_demo.sh`.  No recompile needed.

The same applies to the producer and consumer roles.

## Script interface summary

### Producer / consumer role (`pylabhub_actor`)

```python
import pylabhub_actor as actor

def on_init(api: actor.ActorRoleAPI) -> None:
    """Called once before the loop."""

def on_iteration(slot, flexzone, messages, api: actor.ActorRoleAPI) -> bool:
    """
    Called every iteration.
      slot     — writable (producer) or read-only (consumer) ctypes struct, or None on timeout
      flexzone — persistent flexzone struct, or None
      messages — list of (sender: str, data: bytes)
      api      — ActorRoleAPI proxy
    Return True/None to commit (producer); consumer return value is ignored.
    """

def on_stop(api: actor.ActorRoleAPI) -> None:
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
`hub.pubkey` (CurveZMQ public key) to that directory.  All actors and
processors automatically pick up the endpoint and use encrypted transport.
