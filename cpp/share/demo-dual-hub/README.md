# Dual-Hub Bridge Demo

Demonstrates **cross-hub data flow** using mixed SHM + ZMQ transport.

## Topology

```
Hub A (5570)                                    Hub B (5571)
Producer ─[SHM]─► Processor-A ─[ZMQ PUSH]──►
                               tcp://127.0.0.1:5580
                  Processor-B ─[ZMQ PULL]──► [SHM] ─► Consumer
```

6 processes:
1. **Hub A** — broker at `tcp://0.0.0.0:5570`
2. **Hub B** — broker at `tcp://0.0.0.0:5571`
3. **Producer** — publishes `lab.bridge.raw` on Hub A at 10 Hz (SHM)
4. **Processor-A** — reads SHM from Hub A, pushes via ZMQ to port 5580
5. **Processor-B** — pulls ZMQ from port 5580, writes SHM on Hub B
6. **Consumer** — reads `lab.bridge.processed` from Hub B (SHM)

## Running

```bash
# Build first
cmake --build build

# Run the demo
bash share/demo-dual-hub/run_demo.sh

# Ctrl-C stops all 6 processes
```

## Data flow

- Producer generates: `count` (monotonic), `ts` (Unix time), `value` (sine wave)
- Processor-A passes through unchanged (SHM → ZMQ bridge)
- Processor-B adds `doubled = value * 2.0` (ZMQ → SHM bridge)
- Consumer prints all 4 fields with rate

## Transport config

The key new config fields in `processor.json`:

```json
{
  "in_transport": "zmq",
  "out_transport": "shm",
  "zmq_in_endpoint": "tcp://127.0.0.1:5580",
  "zmq_in_bind": false
}
```

See `processor-a/processor.json` and `processor-b/processor.json` for examples.
