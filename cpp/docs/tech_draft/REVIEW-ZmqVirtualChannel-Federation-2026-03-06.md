# Code + HEP Review: ZMQ Virtual Channel Node & Hub Federation Broadcast

**Date**: 2026-03-06
**Scope**: HEP-CORE-0021, HEP-CORE-0022, hub_zmq_queue.cpp/hpp, hub_producer.cpp/hpp,
hub_consumer.cpp/hpp, broker_service.cpp, processor_config.hpp/cpp,
processor_script_host.cpp, test_datahub_hub_zmq_queue.cpp,
test_datahub_zmq_virtual_channel.cpp, test_datahub_hub_federation.cpp
**Status**: ❌ OPEN — 6 issues found, fixes applied inline

---

## Summary

This review cross-checks HEP-CORE-0021 (ZMQ Virtual Channel Node) and
HEP-CORE-0022 (Hub Federation Broadcast) against their implementations.
Previous session's 22 ZmqQueue/broker fixes (ZQ1–ZQ10, BR1–BR7, PC1–PC2)
are verified correct in code. Six new issues are identified below.

**Test baseline**: 882/882 passing (1 pre-existing flake:
`ProcessorHandlerRemoval` fails only under parallel load, passes alone).

---

## Status Table

| # | Severity | Component | Issue | Status |
|---|----------|-----------|-------|--------|
| D1 | Major | HEP-0022 §6.2 + §8.3 | `relayed_from` vs `originator_uid` in CHANNEL_EVENT_NOTIFY | ✅ Fixed |
| D2 | Major | test_datahub_hub_federation.cpp | 3 comments reference `relayed_from` not `originator_uid` | ✅ Fixed |
| D3 | Minor | HEP-0021 §4.1 | Queue interface snippet shows `read(duration)`/`write()` instead of actual `read_acquire`/`write_acquire`/`write_commit`/`write_abort` API | ✅ Fixed |
| D4 | Minor | HEP-0021 §7.3 | Replacement field list omits `zmq_out_bind` which exists in code | ✅ Fixed |
| D5 | Minor | test_datahub_hub_zmq_queue.cpp line 37 | Comment says "30000 + (pid % 10000)" but code uses "40000 + (pid % 5000)" | ✅ Fixed |
| D6 | Minor | MEMORY.md HEP index | Duplicate HEP-0021/0022 entries with stale "Draft" status | ✅ Fixed |

---

## Detailed Findings

### D1 — HEP-0022 field name mismatch: `relayed_from` vs `originator_uid`

**Severity**: Major (HEP-to-code mismatch — user code following the HEP would break)

**Location**: HEP-CORE-0022-Hub-Federation-Broadcast.md §6.2 (sequence diagram) and §8.3 (Python dict example)

**Problem**: The code emits `originator_uid` in `CHANNEL_EVENT_NOTIFY` for both local
(empty string) and relayed (hub UID) origins. This was the correct BR3 fix.
However, HEP-0022 §6.2 showed `relayed_from="HUB-A"` and §8.3 showed
`"relayed_from": "HUB-DEMOA-00000001"`. Any user writing Python scripts from the HEP
would access the wrong key and always get `""`.

**Code** (`broker_service.cpp`):
```cpp
// Local origin (line 1455):
fwd["originator_uid"] = "";   // Empty = originated on this hub

// Relay delivery (line 2012):
fwd["originator_uid"] = originator;  // Non-empty = relayed from federation peer
```

Note: `originator_uid` in `HUB_RELAY_MSG` (§5.3) was already correct.
Only the downstream `CHANNEL_EVENT_NOTIFY` field name was wrong in the HEP.

**Fix**: Updated §6.2 sequence diagram and §8.3 Python dict to use `originator_uid`.
Also clarified §8.3 that `originator_uid` is always present (empty for local).

---

### D2 — Test federation comment staleness

**Severity**: Major (misleading — reader following comments would look for wrong field)

**Location**: `tests/test_layer3_datahub/test_datahub_hub_federation.cpp`

- Line 9 (file docstring): `"with relayed_from"` → `"with originator_uid"`
- Line 312 (test body): `` `relayed_from` field `` → `` `originator_uid` field ``
- Line 377 (test body): `"relayed_from set"` → `"originator_uid set"`

All three comments were left over from before the BR3 rename. Test assertions at
lines 383 and 464 were already correct (`originator_uid`).

**Fix**: Updated all three comments. No code or assertion changes needed.

---

### D3 — HEP-0021 §4.1 Queue interface inaccurate

**Severity**: Minor (documentation inaccuracy — no user-facing data loss)

**Location**: HEP-CORE-0021-ZMQ-Virtual-Channel-Node.md §4.1

**Problem**: The snippet showed a simplified (fictional) `Queue` interface:
```cpp
virtual ReadResult  read(duration)   = 0;
virtual bool        write(const void* data, size_t size) = 0;
```

The actual `hub::Queue` (hub_queue.hpp) uses an acquire/release protocol:
```cpp
virtual const void* read_acquire(milliseconds timeout) noexcept = 0;
virtual void        read_release() noexcept = 0;
virtual void*       write_acquire(milliseconds timeout) noexcept = 0;
virtual void        write_commit() noexcept = 0;
virtual void        write_abort() noexcept = 0;
```

**Fix**: Replaced the fictional snippet with the actual public API.

---

### D4 — HEP-0021 §7.3 omits `zmq_out_bind`

**Severity**: Minor (incomplete field reference)

**Location**: HEP-CORE-0021-ZMQ-Virtual-Channel-Node.md §7.3

**Problem**: The "replaced by" field list said the old 7 fields become just
`out_transport` + `zmq_out_endpoint`. But `zmq_out_bind` is retained in
`ProcessorConfig` (processor_config.hpp line 167) and used by `ProcessorScriptHost`.
The JSON example only showed 2 fields.

**Fix**: Clarified the text to say `zmq_in_*` fields are removed (input is
auto-discovered), while output side keeps `out_transport`, `zmq_out_endpoint`,
and `zmq_out_bind`. Updated the JSON example to show all three retained fields
with a note about the `zmq_out_bind=true` default.

---

### D5 — ZmqQueue test port formula comment stale

**Severity**: Minor (misleading comment)

**Location**: `tests/test_layer3_datahub/test_datahub_hub_zmq_queue.cpp` line 37

**Problem**: Comment said `"30000 + (pid % 10000) * 5 + offset"` but the
actual formula on the next line is `40000 + (pid % 5000) * 5 + offset`.

**Fix**: Updated comment to match code.

---

### D6 — MEMORY.md duplicate HEP-0021/0022 entries

**Severity**: Minor (stale reference could cause confusion)

**Location**: `MEMORY.md` HEP Index section

**Problem**: Two rows for HEP-0021 and two for HEP-0022 — one set with "Draft"
status from the design phase, one with "Implemented". The stale entries were
cleaned up as part of the fix.

**Fix**: Consolidated to single entries (both Implemented).

---

## Code Verification: Previous Session Fixes (ZQ1–ZQ10, BR1–BR7, PC1–PC2)

All fixes from the previous session's code review verified present and correct:

### ZmqQueue (hub_zmq_queue.cpp)

| Fix | Description | Verified |
|-----|-------------|----------|
| ZQ1 | `is_valid_type_str()` validates all 13 types at factory time | ✅ Line 44–51 |
| ZQ4 | Truncation detection: `rc >= max_frame_sz_` → `recv_frame_error_count` | ✅ Line 228–232 |
| ZQ5 | Move-safe recv_thread: captures `ZmqQueueImpl* impl_ptr` not `this` | ✅ Line 559–560 |
| ZQ6 | Rate-limited schema tag mismatch warning (1s TTL) | ✅ Line 258–266 |
| ZQ8 | SNDHWM applied on PUSH socket when `sndhwm > 0` | ✅ Line 536–537 |
| ZQ9 | Pre-allocated ring buffer (`recv_ring_`, `decode_tmp_`, `current_read_buf_`) | ✅ Lines 109–117 |
| ZQ10 | Sequence gap tracking: `expected_seq_`, `seq_initialized_`, `recv_gap_count_` | ✅ Lines 130–132, 270–282 |

### BrokerService federation (broker_service.cpp)

| Fix | Description | Verified |
|-----|-------------|----------|
| BR1 | `hub_connected_notified_` dedup: allows re-fire on reconnect | ✅ Line 154, 1897, 1943 |
| BR3 | `originator_uid` in CHANNEL_EVENT_NOTIFY (empty=local, non-empty=relay) | ✅ Lines 1455, 2012 |
| BR5 | `channel_to_peer_identities_` inverted index for O(1) relay lookup | ✅ Lines 150, 1893, 2048 |
| BR7 | `relay_dedup_queue_` (deque, ordered by expiry) + `relay_dedup_set_` (O(1)) | ✅ Lines 163–164, 2077–2084 |

### Wire format alignment (HEP-0002 §7.1 vs code)

| Aspect | HEP says | Code says | Match |
|--------|----------|-----------|-------|
| Frame structure | msgpack fixarray[4] | `pk.pack_array(4)` + 4 elements | ✅ |
| magic | `0x51484C50` ('PLHQ') | `kFrameMagic = 0x51484C50u` | ✅ |
| schema_tag | bin8 (always 8 bytes) | `pk.pack_bin(8)` | ✅ |
| seq | uint64, monotone | `send_seq_.fetch_add(1, relaxed)` | ✅ |
| Scalar fields | native msgpack type | `pk.pack(*ptr)` per type | ✅ |
| Array/string/bytes | `bin(byte_size)` | `pk.pack_bin(fd.byte_size)` | ✅ |
| Truncation guard | detect `rc >= max_frame_sz_` | ✅ (ZQ4) | ✅ |
| Empty schema → nullptr | factory rejects | `if (schema.empty()) return nullptr` | ✅ |

---

## One Outstanding Observation (Not a Bug)

**ZQ10 unsigned wrap on out-of-order sequence** (hypothetical): If a PUSH socket
is restarted and `seq` jumps back below `expected_seq_`, the expression
`seq - expected_seq_` wraps as unsigned, adding a large value to `recv_gap_count_`.
ZMQ PUSH/PULL is ordered so in-order jumps-back cannot occur in practice. The
counter may be misleading on PUSH restart without consumer reconnect; this is
acceptable and documented behavior.

---

## HEP-0021 Cross-Check Summary

| Section | HEP claim | Code reality | Match |
|---------|-----------|--------------|-------|
| §2 Design Principles | Broker stores ZMQ endpoint, not data | ✅ `zmq_endpoint` in ChannelEntry | ✅ |
| §5.1 REG_REQ `transport` | optional, default "shm" | ✅ `j.value("data_transport", "shm")` | ✅ |
| §5.2 CONSUMER_REG_ACK `zmq_endpoint` | echoed from ChannelEntry | ✅ messenger.cpp | ✅ |
| §7.1 ProducerOptions | `data_transport`, `zmq_node_endpoint` | ✅ hub_producer.hpp lines 231–234 | ✅ |
| §7.2 Consumer::connect() | reads transport from ACK, creates ZmqQueue PULL | ✅ hub_consumer.cpp lines 507–526 | ✅ |
| §7.3 ProcessorScriptHost | uses `producer->queue()` / `consumer->queue()` | ✅ processor_script_host.cpp lines 497, 520 | ✅ |
| §7.3 zmq_out_bind | not mentioned (gap, now fixed D4) | ✅ processor_config.hpp line 167 | Fixed |
| §4.1 Queue interface | fictional API (gap, now fixed D3) | Actual acquire/release API | Fixed |

---

## HEP-0022 Cross-Check Summary

| Section | HEP claim | Code reality | Match |
|---------|-----------|--------------|-------|
| §2 Design: one-hop relay | `relay=true` flag suppresses re-relay | ✅ broker checks before processing | ✅ |
| §5.1 HUB_PEER_HELLO | `hub_uid`, `protocol_version` | ✅ broker_service.cpp line 1866+ | ✅ |
| §5.2 HUB_PEER_HELLO_ACK | `status`, `hub_uid` | ✅ line 1920 | ✅ |
| §5.3 HUB_RELAY_MSG `originator_uid` | non-empty = relay source | ✅ line 2057 | ✅ |
| §5.4 HUB_TARGETED_MSG | `target_hub_uid`, `channel_name`, `payload` | ✅ | ✅ |
| §6.2 CHANNEL_EVENT_NOTIFY field | `originator_uid` (was `relayed_from`) | Fixed D1 | Fixed |
| §8.3 Python dict field | `originator_uid` (was `relayed_from`) | Fixed D1 | Fixed |
| §9 Loop safety: dedup window | 5s TTL, `msg_id` keyed on `originator_uid:seq` | ✅ `relay_dedup_queue_` + `relay_dedup_set_` | ✅ |
| `channel_to_peer_identities_` index | O(1) relay lookup | ✅ BR5 | ✅ |

---

## Files Modified This Review

| File | Change |
|------|--------|
| `docs/HEP/HEP-CORE-0022-Hub-Federation-Broadcast.md` | §6.2: `relayed_from` → `originator_uid`; §8.3: field rename + clarify always-present |
| `docs/HEP/HEP-CORE-0021-ZMQ-Virtual-Channel-Node.md` | §4.1: actual Queue API; §7.3: add `zmq_out_bind` to retained fields |
| `tests/test_layer3_datahub/test_datahub_hub_federation.cpp` | 3 comment renames: `relayed_from` → `originator_uid` |
| `tests/test_layer3_datahub/test_datahub_hub_zmq_queue.cpp` | Port formula comment: "30000 + pid%10000" → "40000 + pid%5000" |
