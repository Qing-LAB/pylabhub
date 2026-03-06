# Code Review: ZmqQueue + Hub Broadcast Mechanism
**Date**: 2026-03-06
**Files reviewed**: `hub_zmq_queue.hpp/cpp`, `hub_producer.cpp`, `hub_consumer.cpp`, `broker_service.cpp`
**HEPs reviewed**: HEP-CORE-0002 §7.1, HEP-CORE-0021, HEP-CORE-0022

---

## Severity Key

- **CRITICAL**: Undefined behavior or data corruption
- **HIGH**: Silent failure, data loss, or functional breakage under realistic conditions
- **MEDIUM**: Logic error that could cause incorrect behavior but is unlikely in normal usage
- **LOW**: Minor inconsistency, performance concern, or documentation problem

---

## Part 1: ZmqQueue (`hub_zmq_queue.hpp/cpp`)

### [HIGH-ZQ1] Unknown type_str causes silent pack/unpack divergence

**File**: `hub_zmq_queue.cpp:114-169` (`pack_field`, `unpack_field`)

`pack_field` has no `else` clause for unknown type strings. If `fd.type_str` is unrecognized (e.g., typo `"float"` instead of `"float32"`), the field is **silently skipped** — nothing is packed. The receiver then sees an array with fewer elements than declared, increments `recv_frame_error_count_`, and discards the frame.

`unpack_field` correctly returns `false` for unknown types, but this can never happen if the sender silently omitted the field (the array size mismatch fires first).

`field_elem_size` (line 45-52) returns `1` for unknown types. This propagates silently into `compute_field_layout`: a bad field gets `offset += 1` and `is_bin=false`, contaminating all subsequent field offsets.

**The factory never validates type strings.** A schema with `{{"float", 1, 0}}` (missing "32") constructs successfully, allocates a 1-byte slot, and sends corrupted frames at runtime.

**Fix**: Add a validation step in both `pull_from` and `push_to` factories that checks every `type_str` against the known set and returns `nullptr` with an error log if any is invalid.

---

### [HIGH-ZQ2] ZmqQueue::start() failure is ignored by caller (hub_producer / hub_consumer)

**File**: `hub_producer.cpp:540`, `hub_consumer.cpp:523`

Both callers do:
```cpp
impl->zmq_queue_->start();
// return value ignored
```

`ZmqQueue::start()` returns `false` on socket bind/connect failure (e.g., address already in use, invalid endpoint). If this fails:
- `zmq_queue_->socket` is null
- `write_acquire()` returns a valid buffer (doesn't check `running_`)
- `write_commit()` checks `!pImpl->socket` and silently drops every frame
- No error is propagated — the caller gets a valid `Producer` or `Consumer` that silently does nothing

**Fix**: Check the return value of `start()` and return `std::nullopt` from `create_from_parts` / `connect_from_parts` on failure.

---

### [HIGH-ZQ3] Header docstring still references removed "Raw mode"

**File**: `hub_zmq_queue.hpp:16-19`

```
*   Raw mode  : payload = bin(item_size)
*   Schema mode: payload = msgpack array of N typed field values
```

Raw mode factories were removed. This creates documentation drift: readers may think raw mode is still accessible, or that the wire format is different from what's actually implemented. The HEP-0002 §7.1 was updated but the header still has the old description.

**Fix**: Remove the raw mode line from the wire format docstring. Update "one of two forms" to just the schema mode.

---

### [MEDIUM-ZQ4] Truncation detection is semantically fragile

**File**: `hub_zmq_queue.cpp:199-204`

```cpp
// ZMQ silently truncates to buffer size — treat exact match as truncation.
if (rc == static_cast<int>(max_frame_sz_))
{
    ++recv_frame_error_count_;
    continue;
}
```

`max_frame_sz_` is conservatively over-estimated (per-scalar: 9 bytes max; per-bin: 5+size; slack=4). A legitimate frame that happens to match `max_frame_sz_` exactly would be incorrectly discarded. While the slack makes this unlikely, it is not impossible for certain schemas.

The more robust check would be `rc >= static_cast<int>(max_frame_sz_)` (drops frames that hit or exceed the buffer), matching ZMQ's actual truncation behavior (rc == buffer size means "at least this many bytes exist"). The current `==` check is actually `>=` in disguise given the overestimate, but the comment is misleading.

**Fix**: Change `==` to `>=` and update the comment to reflect actual ZMQ semantics.

---

### [MEDIUM-ZQ5] Move semantics unsafe for running ZmqQueue (recv_thread captures `this`)

**File**: `hub_zmq_queue.cpp:466`

```cpp
pImpl->recv_thread_ = std::thread([this] { pImpl->run_recv_thread_(); });
```

The lambda captures `this` (the `ZmqQueue*` value object). If the `ZmqQueue` is move-constructed after `start()`, the old `ZmqQueue` object has `pImpl = nullptr` (moved from). The recv_thread still holds the old `this` pointer and calls `this->pImpl->run_recv_thread_()` — a null pointer dereference.

In current usage, `ZmqQueue` is always heap-allocated via `unique_ptr<ZmqQueue>` and only the unique_ptr is moved (not the ZmqQueue value), so this doesn't trigger. But the API doesn't prevent value-moves of a started queue.

**Fix**: Change the lambda to capture `pImpl.get()` (the `ZmqQueueImpl*`) directly:
```cpp
ZmqQueueImpl* impl_ptr = pImpl.get();
pImpl->recv_thread_ = std::thread([impl_ptr] { impl_ptr->run_recv_thread_(); });
```
This makes the thread independent of the `ZmqQueue` object's address.

---

### [MEDIUM-ZQ6] Schema tag mismatch warning fires for every bad frame, no rate limiting

**File**: `hub_zmq_queue.cpp:228-231`

```cpp
LOGGER_WARN("[hub::ZmqQueue] schema tag mismatch on '{}'", queue_name);
```

If a wrong producer connects to the PULL socket, every received frame triggers this warning at full ZMQ speed. This could flood the log at thousands of lines per second and mask other warnings.

**Fix**: Rate-limit this log to once per second (track `last_mismatch_warn_time_` in `ZmqQueueImpl`).

---

### [LOW-ZQ7] `write_acquire()` timeout parameter is silently ignored

**File**: `hub_zmq_queue.cpp:523-527`

```cpp
void* ZmqQueue::write_acquire(std::chrono::milliseconds /*timeout*/) noexcept
{
    if (!pImpl || pImpl->mode != ZmqQueueImpl::Mode::Write) return nullptr;
    return pImpl->write_buf_.data();
}
```

The Queue interface implies timeout is meaningful for back-pressure. ZmqQueue always returns immediately with no back-pressure. Callers that implement flow control using the timeout semantics get none. This is documented (`Returns pointer to the internal send buffer (always succeeds)`), but the silent EAGAIN drop in `write_commit()` means the caller has no way to distinguish "committed and sent" from "committed but dropped".

**No fix required** (by design), but the behavior should be documented explicitly in the API: "Returns immediately; back-pressure is handled by send_drop_count() — callers must poll this counter if they need flow control feedback."

---

### [LOW-ZQ8] ZMQ_SNDHWM is not configurable

**File**: `hub_zmq_queue.cpp:433-435` (PUSH socket creation in `start()`)

The PUSH socket uses ZMQ's default `ZMQ_SNDHWM` of 1000 messages. When the PULL side is disconnected or slow, frames buffer silently in ZMQ's internal queue up to HWM, then drop. For time-sensitive sensor pipelines, this can introduce unexpected latency spikes (up to 1000 frames of buffered delay).

**Fix**: Expose `ZMQ_SNDHWM` as a `ZmqSchemaField`-independent option in the factory. A sensible default would be `1` (no buffering) for latency-sensitive channels.

---

### [LOW-ZQ9] Per-field recv_buf_ entries allocate a new vector<byte> each time

**File**: `hub_zmq_queue.cpp:244`

```cpp
std::vector<std::byte> payload(item_sz, std::byte{0});
```

Each received frame allocates a heap vector. For high-frequency streams (>1 kHz), this is a significant per-frame allocation. `read_acquire()` moves this to `current_read_buf_`, but `recv_buf_` itself is a `std::queue<std::vector<std::byte>>` — no pooling.

**Fix (future)**: Use a pre-allocated ring buffer of `max_depth` slots of `item_sz` bytes each, avoiding per-frame allocation. Not urgent for current use cases.

---

### [LOW-ZQ10] Sequence number never validated; gaps not detectable

**File**: `hub_zmq_queue.cpp:234`

```cpp
// [2] seq — informational, not validated
```

Drop events can only be detected via `recv_overflow_count()` (internal buffer overflow) or `recv_frame_error_count()` (decode failures). Network drops between PUSH and PULL (e.g., if using non-reliable transport paths) leave no counter. The sequence number is available in the frame but unused.

**Fix (future)**: Add a `recv_gap_count()` counter that tracks sequence number gaps.

---

## Part 2: Hub Broadcast Mechanism (`broker_service.cpp`)

### [HIGH-BR1] `on_hub_connected` fires twice for bidirectional federation peers

**File**: `broker_service.cpp` — `handle_hub_peer_hello` and `handle_hub_peer_hello_ack`

When both Hub A and Hub B are configured as peers of each other:
1. Hub A sends `HUB_PEER_HELLO` to Hub B → Hub B fires `on_hub_connected(A)` and sends `HUB_PEER_HELLO_ACK`
2. Hub A receives `HUB_PEER_HELLO_ACK` → Hub A fires `on_hub_connected(B)`
3. Hub B also sends its own `HUB_PEER_HELLO` to Hub A → Hub A fires `on_hub_connected(B)` **again**

For HubScript, this means `on_hub_connected` callback fires twice for the same peer in bidirectional config. Any state tracking in the script (e.g., counting connected peers) would be wrong.

**Fix**: Track which hub_uids have already triggered `on_hub_connected` (add `std::unordered_set<std::string> hub_connected_notified_` in `BrokerServiceImpl`). In both handlers, only call the callback if the uid is not already in the set.

---

### [HIGH-BR2] Re-connecting peer triggers `on_hub_connected` without `on_hub_disconnected`

**File**: `broker_service.cpp` — `handle_hub_peer_hello` (line 1860)

```cpp
inbound_peers_[peer_hub_uid] = {peer_hub_uid, identity_str, relay_channels};
// ... calls on_hub_connected without checking if already registered
```

If a peer crashes and reconnects, it sends `HUB_PEER_HELLO` again. The existing entry is silently overwritten. `on_hub_disconnected` is **not called** before overwriting, so HubScript state tracking sees a connected→connected transition instead of connected→disconnected→connected.

**Fix**: Before overwriting an existing inbound peer entry, call `on_hub_disconnected` if the entry already exists.

---

### [MEDIUM-BR3] Application-layer relay loop is insufficiently mitigated

**File**: `broker_service.cpp` — `relay_notify_to_peers`

The HEP-0022 dedup mechanism prevents a specific `msg_id` from being relayed twice. However, it does not prevent a script-induced loop:

1. Hub A: `api.notify_channel("ch")` → Broker A → relays `HUB_RELAY_MSG` to Hub B
2. Hub B: script's `on_message()` receives the event → calls `api.notify_channel("ch")` → Broker B → relays back to Hub A (if the reverse path exists)
3. Hub A: delivers to script again → infinite loop (different `msg_id`, not caught by dedup)

The dedup only prevents re-relay of the exact same `msg_id`, not application-originated follow-up notifications. The HEP-0022 document acknowledges this as an "application-layer loop risk" but provides no tooling to detect or break it.

**Fix (design)**: Include `originator_uid` in the `CHANNEL_EVENT_NOTIFY` delivered to the local script so the script can decide whether to react. Add it to the `IncomingMessage` fields. Document this clearly.

---

### [MEDIUM-BR4] `handle_hub_targeted_msg`: silent drop if `on_hub_message` not configured

**File**: `broker_service.cpp` — `handle_hub_targeted_msg` (line 1963)

```cpp
void BrokerServiceImpl::handle_hub_targeted_msg(const nlohmann::json& payload)
{
    if (!cfg.on_hub_message) return;  // silent drop
```

If a HUB_TARGETED_MSG arrives but the hub has not set `on_hub_message`, the message is silently discarded — no log, no error, no reply. The sender gets no indication of failure.

**Fix**: Add a `LOGGER_WARN` when `on_hub_message` is not configured. Optionally send a NAK reply.

---

### [MEDIUM-BR5] `relay_notify_to_peers` O(peers × relay_channels) per notification

**File**: `broker_service.cpp` — `relay_notify_to_peers` (line 1979-2004)

Called from `handle_channel_notify_req` and `handle_channel_broadcast_req` on every event. For each event, iterates all inbound peers, and for each peer, searches `relay_channels` linearly. In a hub with 10 peers each subscribing to 100 channels, every local CHANNEL_NOTIFY_REQ does 1000 string comparisons.

**Fix (future)**: Maintain an inverted index: `channel → list<peer_zmq_identity>`. Update on HELLO/BYE. Makes relay O(1) per channel per event.

---

### [LOW-BR6] `CHANNEL_BROADCAST_REQ` relay uses a non-standard event string format

**File**: `broker_service.cpp:1521`

```cpp
relay_notify_to_peers(socket, target_channel, "broadcast:" + message, sender_uid, data_str);
```

The event string relayed to federation peers is `"broadcast:<user_message>"`. There is no documented contract for what event format a receiving hub's script should expect. `CHANNEL_NOTIFY_REQ` events use clean event names (`"start"`, `"stop"`, etc.). The broadcast relay embeds user data in the event field instead of using the payload field.

**Fix**: Use a fixed event name (`"broadcast"`) and put `message` in the payload field. This is consistent with how `handle_hub_relay_msg` constructs the local `CHANNEL_EVENT_NOTIFY` forward.

---

### [LOW-BR7] `relay_dedup_` uses unbounded unordered_map; `prune_relay_dedup` called every 100ms

**File**: `broker_service.cpp:2007-2015`

`prune_relay_dedup` iterates the entire map every ~100ms (each poll cycle). For a hub with heavy relay traffic, the map can grow large between prune cycles and the iteration is O(n). The 5s window means entries live for 50 prune cycles.

**Fix (future)**: Use a `std::deque` of `{expiry, msg_id}` sorted by expiry (insert in order since relay_seq_ is monotone). Prune from the front until `expiry > now`. O(expired) per prune cycle instead of O(all).

---

## Part 3: hub_producer.cpp / hub_consumer.cpp

### [MEDIUM-PC1] Messenger callback TOCTOU race on user callback clearing

**File**: `hub_producer.cpp:484-486`, `hub_consumer.cpp:483-485`

Per-channel Messenger callbacks capture `raw` (the `ProducerImpl*`/`ConsumerImpl*`) and check `raw->closed`:

```cpp
messenger.on_channel_closing(ch, [raw]() {
    if (!raw->closed && raw->on_channel_closing_cb)
        raw->on_channel_closing_cb();
});
```

`close()` clears the Messenger callback first (`messenger->on_channel_closing(ch, nullptr)`) then later sets `pImpl->closed = true` and clears `on_channel_closing_cb = nullptr`.

Race window: Messenger worker thread enters the lambda and passes the `!raw->closed` check (closed=false). Concurrently, `close()` clears `on_channel_closing_cb = nullptr`. Messenger thread then calls `raw->on_channel_closing_cb()` on a null `std::function` → UB.

The race is narrow (requires close() to run between the `!closed` check and the function call), but technically exists. The fix would be a shared mutex protecting the callback, or ensuring the Messenger worker thread drains before `close()` clears user callbacks.

**Fix (pragmatic)**: Copy the callback under a lock before calling:
```cpp
messenger.on_channel_closing(ch, [raw]() {
    std::function<void()> cb;
    { /* lock */ cb = raw->on_channel_closing_cb; }
    if (!raw->closed && cb) cb();
});
```
This requires adding a mutex for user callbacks (or reusing `consumer_list_mu`). Currently a raw pointer guard with no locking.

---

### [MEDIUM-PC2] `Consumer::is_stopping()` has inverted semantics vs Producer

**File**: `hub_consumer.cpp:784-787` vs `hub_producer.cpp:818-821`

```cpp
// Consumer
bool Consumer::is_stopping() const noexcept {
    return pImpl && !pImpl->running.load(std::memory_order_relaxed);
}

// Producer
bool Producer::is_stopping() const noexcept {
    return pImpl && pImpl->write_stop.load(std::memory_order_relaxed);
}
```

Before `start()` is called, `Consumer::is_stopping()` returns `true` (since `running` defaults to false). `Producer::is_stopping()` returns `false` (write_stop defaults to false). Same function, opposite semantics in the pre-start state.

Script hosts call `is_stopping()` to check whether to exit processing loops. If a consumer calls this before `start()`, it would think it's stopping. This is academic (scripts run after `start()`), but the semantic inconsistency is a maintenance hazard.

**Fix**: Add a `stop_requested_` atomic flag to `ConsumerImpl` mirroring `write_stop` in `ProducerImpl`, and base `is_stopping()` on that flag.

---

### [LOW-PC3] `Producer::stop()` doesn't null ZmqQueue after stopping it

**File**: `hub_producer.cpp:663-666`

```cpp
if (pImpl->zmq_queue_)
{
    pImpl->zmq_queue_->stop();
}
```

After `stop()`, the ZmqQueue is stopped but the pointer is still non-null. A subsequent `start()` call (if `running` were reset) would try to `start()` an already-stopped queue. However, `ZmqQueue::start()` checks `running_.exchange(true)` and returns false if already "running" — but after `stop()`, `running_` is false, so `start()` would try to create a new ZMQ context while the old socket pointers are null. `start()` would succeed. This isn't a bug in current code (Producer::start() is not re-entrant), but it's fragile.

---

### [LOW-PC4] ZMQ schema is not registered with the broker for cross-validation

**File**: `hub_producer.cpp:519-542`, `hub_consumer.cpp:498-525`

When `data_transport="zmq"`, the ZmqQueue's schema (`zmq_schema`) is configured independently by producer and consumer. The broker stores the channel's BLDS schema (for SHM), but there is no broker-level validation that the consumer's `zmq_schema` matches the producer's `zmq_schema`.

A consumer with a wrong schema will silently drop all frames (schema_tag mismatch if enabled, or field-count mismatch if not). The operator has no visibility into this mismatch from the broker's perspective.

**Fix (future)**: Add a `zmq_schema_hash` field to `REG_REQ` for ZMQ channels. Include it in `DISC_ACK` so the consumer can validate before connecting. Tracked as deferred in HEP-0023.

---

## Part 4: Obsolete / Duplicate Code

### [LOW-OB1] `PendingCtrlSend::type` unused for data frames

**File**: `hub_producer.cpp:47-53`

```cpp
struct PendingCtrlSend
{
    std::string            identity;
    std::string            type;     // Ctrl type string (ignored for data frames)
    ...
    bool                   is_data{false};
};
```

When `is_data=true`, `type` is an empty string passed to `drain_ctrl_send_queue_()` which ignores it. The field is wasted memory for every data frame queued. Minor but adds confusion to the struct layout.

---

### [LOW-OB2] `recv_and_dispatch_data_` return-true-on-malformed in both Consumer and embedded mode

**File**: `hub_consumer.cpp:165-168`

```cpp
if (frames.size() < 2 || frames[0].size() < 1)
{
    return true; // Consumed but malformed
}
```

Returning `true` for malformed frames means the bounded-loop caller (`handle_data_events_nowait`) continues consuming. If the DEALER receives a burst of malformed frames, the loop runs up to `kMaxRecvBatch=100` iterations processing them. This is intentional (prevent queue buildup) but the comment `// Consumed a message (queue decremented)` in `ProducerImpl::recv_and_dispatch_ctrl_` is clearer. The consumer's comment is just `// Consumed but malformed`. The pattern is consistent but worth auditing: make sure all return-true-on-malformed cases are intentional, not copy-paste without understanding.

---

## Summary Table

**Status key**: ✅ Fixed | 🔄 Deferred (tracked in HEP-0023)

| ID | Severity | Area | Summary | Status |
|----|----------|------|---------|--------|
| ZQ1 | HIGH | ZmqQueue | Unknown type_str silently corrupts schema layout at factory time | ✅ Fixed 2026-03-06 |
| ZQ2 | HIGH | ZmqQueue | `start()` return value ignored; failed socket silently drops all data | ✅ Fixed 2026-03-06 |
| ZQ3 | HIGH | ZmqQueue | Header docstring still says "Raw mode" (removed feature) | ✅ Fixed 2026-03-06 |
| ZQ4 | MEDIUM | ZmqQueue | Truncation detection uses `==` instead of `>=` | ✅ Fixed 2026-03-06 |
| ZQ5 | MEDIUM | ZmqQueue | Move of running ZmqQueue causes recv_thread null-ptr deref | ✅ Fixed 2026-03-06 |
| ZQ6 | MEDIUM | ZmqQueue | Schema tag mismatch warning is not rate-limited | ✅ Fixed 2026-03-06 |
| ZQ7 | LOW | ZmqQueue | `write_acquire()` timeout silently ignored; no back-pressure | ✅ Fixed 2026-03-06 |
| ZQ8 | LOW | ZmqQueue | ZMQ_SNDHWM not configurable; default 1000-frame buffer hides latency | ✅ Fixed 2026-03-06 |
| ZQ9 | LOW | ZmqQueue | Per-frame vector allocation in recv path; no pooling | ✅ Fixed 2026-03-06 |
| ZQ10 | LOW | ZmqQueue | Sequence gaps not detected or counted | ✅ Fixed 2026-03-06 |
| BR1 | HIGH | Broadcast | `on_hub_connected` fires twice for bidirectional federation peers | ✅ Fixed 2026-03-06 |
| BR2 | HIGH | Broadcast | Peer reconnect overwrites entry without calling `on_hub_disconnected` | ✅ Fixed 2026-03-06 |
| BR3 | MEDIUM | Broadcast | Application-layer relay loop not detectable; no originator in script events | ✅ Fixed 2026-03-06 |
| BR4 | MEDIUM | Broadcast | Silent drop of targeted messages when `on_hub_message` not configured | ✅ Fixed 2026-03-06 |
| BR5 | MEDIUM | Broadcast | `relay_notify_to_peers` O(peers × channels) per event | ✅ Fixed 2026-03-06 |
| BR6 | LOW | Broadcast | Broadcast relay uses non-standard `"broadcast:<msg>"` event string | ✅ Fixed 2026-03-06 |
| BR7 | LOW | Broadcast | `relay_dedup_` prune is O(all entries) every poll cycle | ✅ Fixed 2026-03-06 |
| PC1 | MEDIUM | Producer/Consumer | TOCTOU race: Messenger callback vs `close()` clearing user callback | ✅ Fixed 2026-03-06 |
| PC2 | MEDIUM | Producer/Consumer | `Consumer::is_stopping()` semantics inverted vs Producer before `start()` | ✅ Fixed 2026-03-06 |
| PC3 | LOW | Producer | ZmqQueue not nulled after `stop()` | ✅ Fixed 2026-03-06 |
| PC4 | LOW | Producer/Consumer | ZMQ schema not propagated through broker; no cross-validation at connect | 🔄 Deferred — HEP-0023 |
| OB1 | LOW | Producer | `PendingCtrlSend::type` unused for data frames | ✅ Fixed 2026-03-06 |
| OB2 | LOW | Consumer | return-true-on-malformed pattern needs comment consistency review | ✅ Fixed 2026-03-06 |

---

## Resolution Summary (2026-03-06)

All 22 actionable issues fixed. PC4 deferred to HEP-0023 by design (requires broker protocol change).

**ZmqQueue fixes**:
- Factory validation (`find_invalid_type` + `is_valid_type_str`) eliminates silent schema corruption
- Ring buffer (`recv_ring_`, `decode_tmp_`, `current_read_buf_`) eliminates per-frame heap allocation
- `recv_gap_count()` counter tracks ZMQ sequence gaps
- Rate-limited schema tag mismatch warning (1/second via `last_mismatch_warn_`)
- `sndhwm` parameter on `push_to()` exposes ZMQ_SNDHWM for latency-sensitive pipelines
- Thread-safe move: lambda captures `impl_ptr = pImpl.get()` not `this`

**Broadcast / Broker fixes**:
- `hub_connected_notified_` set prevents double `on_hub_connected` in bidirectional federation
- Reconnect now calls `on_hub_disconnected` before overwriting peer entry
- `originator_uid` field in relayed CHANNEL_EVENT_NOTIFY enables application-layer loop detection
- Inverted index `channel_to_peer_identities_` makes relay O(1) per channel
- Deque+set dedup (`relay_dedup_queue_` + `relay_dedup_set_`) makes prune O(expired)

**Producer/Consumer fixes**:
- `callbacks_mu` mutex + copy-before-invoke in Messenger lambdas eliminates TOCTOU race
- `stop_requested_` atomic gives `Consumer::is_stopping()` consistent semantics with Producer

**Tests**: 882 total, 881 pass. 1 pre-existing flake (`ProcessorHandlerRemoval` — intermittent under parallel load, passes alone). 5 new data-integrity tests added for ZmqQueue covering sizes 1B–16KB across all alignment boundaries.
