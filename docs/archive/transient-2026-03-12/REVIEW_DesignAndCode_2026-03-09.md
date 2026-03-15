# Code Review: Design vs Code Consistency
**Date**: 2026-03-09 (formal triage 2026-03-10)
**Scope**: broker_service, channel_registry, messenger, hub::Producer/Consumer callbacks, script config
**Status**: ✅ CLOSED 2026-03-10 — DC-04/DC-06 deferred (intentional); all other items fixed

Triage source: `docs/code_review/review_design_and_code.md`

---

## Status Table

| ID | Sev | Finding | Status |
|----|-----|---------|--------|
| DC-01 | P1 | HEP-0019 METRICS_REQ SHM merge not implemented | ✅ FIXED 2026-03-10 |
| DC-02 | P1 | HEP-0022 inbound federation peer UID not validated | ✅ FIXED (pre-existing, verified 2026-03-10) |
| DC-03 | P1 | hub callback std::function race | ✅ FIXED (pre-existing, verified 2026-03-10) |
| DC-04 | P2 | Script config type defaults silently to Python | ⚪ DEFERRED (doc-only, Python is only type) |
| DC-05 | P2 | Consumer registry accumulates duplicate/stale entries | ✅ FIXED (zmq_identity dedup key, 2026-03-09) |
| DC-06 | P2 | Messenger API redundancy (old register_* + new create/connect_channel) | ⚪ DEFERRED (legacy surface, not harmful) |

---

## DC-01 — HEP-0019 METRICS_REQ SHM merge not implemented
**File**: `src/utils/ipc/broker_service.cpp`, line 691 (handle_metrics_req)
**Severity**: P1 — design gap
**Status**: ❌ OPEN

HEP-CORE-0019 §3.2 requires `METRICS_REQ` to return a merged view combining:
- In-memory broker metrics (`metrics_store_` — role-reported heartbeat metrics)
- SHM-derived ring-buffer metrics (read live from the DataBlock segment)

Current implementation (`handle_metrics_req` → `query_metrics`) only returns `metrics_store_`.
SHM block data is available via the separate `SHM_BLOCK_QUERY_REQ` endpoint, which calls
`query_shm_blocks()` and reads SHM via `datablock_get_metrics()`.

Two possible resolutions:
1. **Merge**: In `query_metrics(channel)`, after building the in-memory view, call `query_shm_blocks(channel)` and append `shm_blocks` key to the response. This fully satisfies HEP-0019.
2. **Specify two-endpoint design**: Formally amend HEP-0019 to state that SHM block metrics are available via `SHM_BLOCK_QUERY_REQ` and `METRICS_REQ` covers only role-reported in-memory metrics.

Option 1 is preferred (spec compliance). The cost is low: `query_shm_blocks()` already exists and opens SHM read-only. The merge can be gated on whether a `channel_name` is specified in the request.

**Recommended fix** (for Option 1):
```cpp
// In handle_metrics_req():
nlohmann::json resp = query_metrics(channel);
if (!channel.empty()) {
    resp["shm_blocks"] = query_shm_blocks(channel);  // merge live SHM data
}
return resp;
```

---

## DC-02 — HEP-0022 inbound federation peer UID not validated
**File**: `src/utils/ipc/broker_service.cpp`, lines 2128–2157
**Severity**: P1 — was a security gap
**Status**: ✅ FIXED (pre-existing)

The `handle_hub_peer_hello()` handler (line 2128) now validates `peer_hub_uid` against
`cfg.peers` before accepting a connection. Unknown UIDs receive a `HUB_PEER_HELLO_ACK`
with `"status": "error"` and `"reason": "hub_uid not in configured peers"`.

The informal review_design_and_code.md finding was stale — this had been fixed before
the formal triage. Confirmed by reading `broker_service.cpp:2137–2157`.

---

## DC-03 — hub callback std::function thread-safety
**File**: `src/utils/hub/hub_producer.cpp`, `src/utils/hub/hub_consumer.cpp`
**Severity**: P1 — was a race condition
**Status**: ✅ FIXED (pre-existing)

`hub::Producer` and `hub::Consumer` protect all callback storage and invocation via
`callbacks_mu` (`std::mutex`). Setters lock before writing; invocations copy-under-lock
before calling. Pattern: `{ std::lock_guard lk(callbacks_mu); cb = on_xyz_cb; } if(cb) cb(...);`

Confirmed in `hub_producer.cpp:71` (callbacks_mu declaration), lines 236–268 (copy-under-lock),
lines 584–625 (setter locking). The informal review finding was stale.

---

## DC-04 — Script config type defaults silently to Python
**Files**: `src/producer/producer_config.cpp:185`, `src/consumer/consumer_config.cpp:112`, `src/processor/processor_config.cpp:250`
**Severity**: P2
**Status**: ⚪ DEFERRED

`producer_config.cpp:185`: `cfg.script_type = j["script"].value("type", std::string{"python"});`

HEP-0011 §3 says the `"type"` field should be explicit when a script block is present.
Defaulting to `"python"` silently ignores misconfigured type strings (e.g., `"Type": "python"`
would be silently accepted as "python" default rather than rejected).

Deferred because Python is the only currently supported script type; a strict validation
would break all existing configs that omit `"type"`. Revisit when a second script type is added.
At that point: LOGGER_WARN if "type" is absent from a script block.

---

## DC-05 — Consumer registry accumulates duplicate/stale entries
**File**: `src/utils/ipc/channel_registry.cpp:59–78`
**Severity**: P2
**Status**: ✅ FIXED (2026-03-09)

`ChannelRegistry::register_consumer()` now deduplicates by `zmq_identity` before
`push_back()`. A reconnecting consumer (same DEALER socket identity) replaces its stale
entry rather than accumulating a duplicate. This handles the most common reconnect race.

Note: A consumer with a NEW ZMQ identity (process restart) will get a second entry until
the old one is removed by heartbeat timeout. This is acceptable — the heartbeat dead-detection
path calls `deregister_consumer()` for timed-out entries.

---

## DC-06 — Messenger API redundancy
**Files**: `src/include/utils/messenger.hpp:199–205`, `src/utils/ipc/messenger_protocol.cpp`
**Severity**: P2
**Status**: ⚪ DEFERRED

`Messenger` exposes two tiers:
- **Old tier**: `register_producer()`, `register_consumer()`, `deregister_consumer()`, `discover_producer()` — thin wrappers sending raw protocol frames without full field sets
- **New tier**: `create_channel()`, `connect_channel()` — high-level with retry, full payload, `ChannelHandle` return

The old tier is now only used by `hub_consumer.cpp:923` (deregister_consumer) and legacy examples.
The registration paths all go through the new tier.

Deferred: no functional bug; cleanup would involve deprecating old APIs. Low priority.
