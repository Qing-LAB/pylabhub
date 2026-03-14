# Archive: Actor Elimination Design Revision — 2026-03-01

**Reason:** Architectural decision to eliminate `pylabhub-actor` and replace the
multi-role actor model with standalone `pylabhub-producer` and `pylabhub-consumer`
binaries. Each component now owns its own directory, config, vault, script, and PID
lock — mirroring the existing `pylabhub-processor` standalone pattern.

## Archived documents

| Document | Reason |
|----------|--------|
| `HEP-CORE-0010-Actor-Thread-Model-and-Unified-Script-Interface.md` | Actor eliminated. Thread model for producer/consumer lives in HEP-CORE-0018. |
| `HEP-CORE-0012-Processor-Role.md` | ProcessorRole inside actor eliminated. Standalone processor is HEP-CORE-0015. |
| `HEP-CORE-0014-Actor-Framework-Design.md` | Actor framework eliminated. Superseded by HEP-CORE-0018 (Producer and Consumer Binaries). |
| `REVISION_SUMMARY.md` | AI-generated transient session summary; no canonical content. |

## What replaced them

| Archived | Replaced by |
|----------|-------------|
| HEP-CORE-0010 | HEP-CORE-0018 §7 (thread model for producer/consumer) |
| HEP-CORE-0012 | HEP-CORE-0015 (standalone processor binary) |
| HEP-CORE-0014 | HEP-CORE-0018 (producer and consumer binaries) |
| REVISION_SUMMARY.md | — (dropped) |

## Design decision record

The actor container provided shared identity across multiple roles. This created:

1. Multi-broker identity ambiguity (producer on Hub A, consumer on Hub B, same actor UID)
2. Multi-machine deployment confusion (same dir claimed by processes on different hosts)
3. PID lock incompatibility (one PID file, potentially multiple processes)
4. Inconsistency with the standalone processor model

Resolution: every data-plane component gets its own directory, its own UID, and its
own process. Fan-in/fan-out via multiple independent binaries. Transformation via
`pylabhub-processor`. No multi-role container.
