# Archive: transient-2026-03-03

## Archived Documents

| Document | Reason |
|----------|--------|
| `HEP-CORE-0005-script-interface-framework.md` | Superseded by HEP-CORE-0011 (ScriptHost Abstraction Framework); all content obsolete |

## Context

HEP-CORE-0005 proposed an abstract `IScriptEngine` / `IScriptContext` / `ScriptValue` interface
for script integration. The actual implementation took a fundamentally different approach:
- Direct pybind11 embedding instead of abstract engine layer
- No type-erased `ScriptValue`; Python uses native types, Lua uses stack
- `ScriptHost` base class owns lifecycle + thread model only (HEP-CORE-0011)

The document was already marked **Superseded** in its header. This archive completes the cleanup.

## Concurrent Cleanup

Actor terminology scrubbed from all remaining HEP files (HEP-0002, 0006, 0008, 0009, 0011,
0013, 0015, 0016, 0017, 0018). References updated to reflect standalone binary architecture
(producer/consumer/processor binaries instead of actor container).
