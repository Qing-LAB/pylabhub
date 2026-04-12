# Implementation Plan: Band Pub/Sub Messaging

**Status**: Plan (2026-04-10)
**Prereq**: `channel_redesign.md` (design rationale)
**Branch**: `feature/lua-role-support`

---

## 1. Overview

Implement the band as a broker-hosted pub/sub messaging group. This
involves changes to three layers:

1. **Broker (BrokerService)**: New registry, new message handlers, fan-out
2. **Role client (BrokerRequestComm)**: New API methods
3. **Role framework (RoleAPIBase)**: Script-visible API, notification routing

No new ZMQ sockets. All band messages flow through the existing broker
DEALER socket that `BrokerRequestComm` already owns.

---

## 2. Broker Side

### 2.1 New data structure: BandRegistry

File: `src/utils/ipc/band_registry.hpp` (new, broker-internal)

```cpp
namespace pylabhub::broker
{

struct BandMember
{
    std::string role_uid;
    std::string role_name;
    std::string zmq_identity;   // ROUTER identity for push notifications
    std::chrono::steady_clock::time_point joined_at;
};

struct Band
{
    std::string name;           // "#band_name@hub"
    std::vector<BandMember> members;
    std::chrono::steady_clock::time_point created_at;
};

/// Separate from ChannelRegistry (which is about data plane registration).
/// This registry manages lightweight pub/sub messaging groups.
class BandRegistry
{
public:
    /// Join a band. Creates the band if it doesn't exist.
    /// Returns the current member list (including the new member).
    nlohmann::json join(const std::string &band,
                        const std::string &role_uid,
                        const std::string &role_name,
                        const std::string &zmq_identity);

    /// Leave a band. Removes the member. Deletes band if empty.
    /// Returns true if the member was found and removed.
    bool leave(const std::string &band, const std::string &role_uid);

    /// Remove a member from ALL bands (called on heartbeat timeout).
    /// Returns list of {band, remaining_members} for notification.
    std::vector<std::pair<std::string, std::string>>
    remove_from_all(const std::string &role_uid);

    /// Get member list for a band.
    std::optional<nlohmann::json> members(const std::string &band) const;

    /// Get all members of a band as zmq_identity strings (for fan-out).
    std::vector<std::string> member_identities(
        const std::string &band,
        const std::string &exclude_uid = {}) const;

    /// Check if a band exists.
    bool exists(const std::string &band) const;

private:
    std::unordered_map<std::string, Band> groups_;
};

} // namespace pylabhub::broker
```

No mutex — accessed only from the broker run() thread (same discipline as
existing ChannelRegistry).

### 2.2 New message handlers in BrokerServiceImpl

File: `src/utils/ipc/broker_service.cpp`

Add to `process_message()` dispatch:

```cpp
else if (msg_type == "BAND_JOIN_REQ")
{
    auto resp = handle_band_join_req(payload, identity);
    send_reply(socket, identity,
        resp.value("status","") == "success" ? "BAND_JOIN_ACK" : "ERROR", resp);
}
else if (msg_type == "BAND_LEAVE_REQ")
{
    auto resp = handle_band_leave_req(payload);
    send_reply(socket, identity,
        resp.value("status","") == "success" ? "BAND_LEAVE_ACK" : "ERROR", resp);
}
else if (msg_type == "BAND_BROADCAST_REQ")
{
    handle_band_broadcast_req(socket, payload, identity);
    // fire-and-forget — no reply
}
else if (msg_type == "BAND_MEMBERS_REQ")
{
    auto resp = handle_band_members_req(payload);
    send_reply(socket, identity, "BAND_MEMBERS_ACK", resp);
}
```

### 2.3 Handler implementations

**handle_band_join_req:**

```
Input:  {"band": "#name@hub", "role_uid": "...", "role_name": "..."}
Output: {"status": "success", "band": "#name@hub", "members": [...]}

1. band_registry_.join(band, role_uid, role_name, identity_str)
2. Fan out BAND_JOIN_NOTIFY to all existing members (before adding new member):
   For each member in band (excluding the joiner):
     send_to_identity(socket, member.zmq_identity, "BAND_JOIN_NOTIFY",
       {"band": ..., "role_uid": joiner_uid, "role_name": joiner_name})
3. Return ACK with current member list (including new member)
```

**handle_band_leave_req:**

```
Input:  {"band": "#name@hub", "role_uid": "..."}
Output: {"status": "success"}

1. band_registry_.leave(band, role_uid)
2. Fan out BAND_LEAVE_NOTIFY to remaining members:
   For each remaining member:
     send_to_identity(socket, member.zmq_identity, "BAND_LEAVE_NOTIFY",
       {"band": ..., "role_uid": leaver_uid, "reason": "voluntary"})
3. If band is now empty, it's auto-deleted (no notification needed)
```

**handle_band_broadcast_req:**

```
Input:  {"band": "#name@hub", "sender_uid": "...", "body": {...}}

1. Get member identities excluding sender
2. For each member:
     send_to_identity(socket, member.zmq_identity, "BAND_BROADCAST_NOTIFY",
       {"band": ..., "sender_uid": ..., "body": {...}})
3. No reply (fire-and-forget)
```

**handle_band_members_req:**

```
Input:  {"band": "#name@hub"}
Output: {"band": ..., "members": [{"role_uid": ..., "role_name": ...}, ...]}

1. band_registry_.members(band)
2. Return member list (or empty array if band doesn't exist)
```

### 2.4 Heartbeat-based auto-leave

In `check_heartbeat_timeouts()` (existing method), when a role times out:

```
1. Find all bands the role belongs to:
   auto affected = band_registry_.remove_from_all(role_uid)
2. For each affected band:
   Fan out BAND_LEAVE_NOTIFY with reason "heartbeat_timeout"
```

This requires mapping role_uid → ZMQ identity for the timed-out role.
The existing ChannelEntry already has `producer_role_uid` and ConsumerEntry
has `role_uid`. The heartbeat timeout handler can look up the identity.

### 2.5 Wire format summary

All messages use the existing 4-frame ROUTER format:
```
[identity] ['C'] [msg_type] [json_body]
```

| msg_type | Direction | JSON fields |
|----------|-----------|-------------|
| BAND_JOIN_REQ | Role→Broker | band, role_uid, role_name |
| BAND_JOIN_ACK | Broker→Role | status, band, members[] |
| BAND_LEAVE_REQ | Role→Broker | band, role_uid |
| BAND_LEAVE_ACK | Broker→Role | status |
| BAND_BROADCAST_REQ | Role→Broker | band, sender_uid, body{} |
| BAND_MEMBERS_REQ | Role→Broker | band |
| BAND_MEMBERS_ACK | Broker→Role | band, members[] |
| BAND_JOIN_NOTIFY | Broker→Role | band, role_uid, role_name |
| BAND_LEAVE_NOTIFY | Broker→Role | band, role_uid, reason |
| BAND_BROADCAST_NOTIFY | Broker→Role | band, sender_uid, body{} |

---

## 3. Role Client Side (BrokerRequestComm)

### 3.1 New API methods

File: `src/include/utils/broker_request_comm.hpp`

```cpp
// ── Band pub/sub messaging ───────────────────────────────────────────

/// Join a band (auto-creates if needed). Returns member list.
[[nodiscard]] std::optional<nlohmann::json>
band_join(const std::string &band, int timeout_ms = 5000);

/// Leave a band.
bool band_leave(const std::string &band, int timeout_ms = 5000);

/// Send JSON message to all band members (fire-and-forget).
void band_broadcast(const std::string &band,
                    const nlohmann::json &body);

/// Query current member list.
[[nodiscard]] std::optional<nlohmann::json>
band_members(const std::string &band, int timeout_ms = 5000);
```

### 3.2 Implementation

File: `src/utils/network_comm/broker_request_comm.cpp`

```cpp
std::optional<nlohmann::json>
BrokerRequestComm::band_join(const std::string &band, int timeout_ms)
{
    nlohmann::json payload;
    payload["band"] = band;
    payload["role_uid"] = /* from config or set_uid */;
    payload["role_name"] = /* from config or set_name */;
    return pImpl->do_request("BAND_JOIN_REQ", "BAND_JOIN_ACK",
                             std::move(payload), timeout_ms);
}

bool BrokerRequestComm::band_leave(const std::string &band, int timeout_ms)
{
    nlohmann::json payload;
    payload["band"] = band;
    payload["role_uid"] = /* from config */;
    auto result = pImpl->do_request("BAND_LEAVE_REQ", "BAND_LEAVE_ACK",
                                    std::move(payload), timeout_ms);
    return result.has_value() && result->value("status", "") == "success";
}

void BrokerRequestComm::band_broadcast(const std::string &band,
                                           const nlohmann::json &body)
{
    nlohmann::json payload;
    payload["band"] = band;
    payload["sender_uid"] = /* from config */;
    payload["body"] = body;
    pImpl->cmd_queue.push(SendCmd{"BAND_BROADCAST_REQ", std::move(payload)});
}

std::optional<nlohmann::json>
BrokerRequestComm::band_members(const std::string &band,
                                    int timeout_ms)
{
    nlohmann::json payload;
    payload["band"] = band;
    return pImpl->do_request("BAND_MEMBERS_REQ", "BAND_MEMBERS_ACK",
                             std::move(payload), timeout_ms);
}
```

Note: `role_uid` and `role_name` need to be available to BrokerRequestComm.
Options:
- (a) Store them in BrokerRequestComm::Config (set during connect)
- (b) Pass them as parameters to each method
- (c) BrokerRequestComm holds a reference to RoleAPIBase identity

Option (a) is cleanest — add `role_uid` and `role_name` to Config.

### 3.3 Notification routing

Band notifications (BAND_JOIN_NOTIFY, BAND_LEAVE_NOTIFY,
BAND_BROADCAST_NOTIFY) arrive on the broker DEALER socket and are dispatched
by `recv_and_dispatch()` → `on_notification_cb`. RoleAPIBase must wire
this callback to route notifications to `core_.enqueue_message()`.

This is the existing `on_notification()` callback that was identified as
MISSING-002 in the code review — it needs to be wired now.

---

## 4. Role Framework (RoleAPIBase)

### 4.1 Script-visible API

File: `src/include/utils/role_api_base.hpp`

```cpp
// ── Band pub/sub ────────────────────────────────────────────────────

/// Join a named band. Auto-creates if it doesn't exist.
/// Returns member list JSON, or nullopt on failure.
[[nodiscard]] std::optional<nlohmann::json>
band_join(const std::string &band);

/// Leave a band.
bool band_leave(const std::string &band);

/// Send JSON message to all band members.
void band_broadcast(const std::string &band,
                    const nlohmann::json &body);

/// Query band member list.
[[nodiscard]] std::optional<nlohmann::json>
band_members(const std::string &band);
```

Implementation forwards to `broker_channel_->band_join()` etc.

### 4.2 Notification wiring

In `start_broker_thread()` (or a new `wire_broker_notifications()` method),
wire `broker_channel_->on_notification()` to dispatch band notifications:

```cpp
broker_channel_->on_notification([core](const std::string &type,
                                         const nlohmann::json &body) {
    IncomingMessage msg;
    msg.event = type;  // "BAND_JOIN_NOTIFY", "BAND_BROADCAST_NOTIFY", etc.
    msg.details = body;
    core->enqueue_message(std::move(msg));
});
```

Scripts receive these as entries in the `msgs` parameter of their callback.

### 4.3 Python/Lua/Native API registration

Each API class (ProducerAPI, ConsumerAPI, ProcessorAPI) registers:

```python
api.band_join("#sensor_sync")
api.band_leave("#sensor_sync")
api.band_broadcast("#sensor_sync", {"event": "calibration_done"})
members = api.band_members("#sensor_sync")
```

---

## 5. Thread Model

No new threads. All channel communication uses the existing broker thread:

```
Data loop thread:
    script calls api.band_broadcast("#band", body)
    → RoleAPIBase::band_broadcast()
    → broker_channel_->band_broadcast()  (enqueues SendCmd)
    → MonitoredQueue push → signal socket wakes broker thread

Broker thread (BrokerRequestComm poll loop):
    drain command queue → send BAND_BROADCAST_REQ on DEALER
    poll DEALER → recv BAND_BROADCAST_NOTIFY from broker
    → on_notification_cb → core_.enqueue_message()

Data loop thread (next cycle):
    msgs = core_.drain_messages()
    → script receives {"event": "BAND_BROADCAST_NOTIFY", "details": {...}}
```

---

## 6. Implementation Order

### Step 1: BandRegistry (broker-internal)
- New file: `src/utils/ipc/band_registry.hpp`
- New file: `src/utils/ipc/band_registry.cpp`
- Add to CMakeLists.txt
- Unit tests: L3 (join/leave/broadcast through real broker)

### Step 2: Broker message handlers
- Add dispatch entries in `process_message()`
- Implement 4 handlers in broker_service.cpp
- Add heartbeat auto-leave in `check_heartbeat_timeouts()`
- Test: L3 integration (role sends JOIN/LEAVE/BROADCAST, verify fan-out)

### Step 3: BrokerRequestComm API
- Add 4 methods to header + implementation
- Add `role_uid`/`role_name` to Config
- Test: L3 (BrokerRequestComm → BrokerService round-trip)

### Step 4: RoleAPIBase + notification wiring
- Add band methods to RoleAPIBase
- Wire `on_notification()` callback in `start_broker_thread()`
- Test: L3 (two roles, one sends msg, other receives notification)

### Step 5: Script API
- Register band methods in ProducerAPI/ConsumerAPI/ProcessorAPI
- Lua engine: add band functions
- Native engine: add band function pointers
- Test: L2 engine tests, L4 integration

### Step 6: Migrate old channel usage
- `api.broadcast_channel()` → `api.band_broadcast()`
- `api.notify_channel()` → `api.band_broadcast()` with event field
- Remove old Messenger channel_notify/channel_broadcast methods
- Update HEP-CORE-0007, HEP-CORE-0018

### Step 7: Remove old P2C infrastructure
- Remove `start_comm_thread()` from RoleAPIBase
- Remove ChannelHandle, ChannelPattern
- Remove P2C socket creation from Messenger
- Remove Producer/Consumer P2C threads
- Remove `handle_*_events_nowait()` methods

### Step 8: Full HEP cleanup pass (MANDATORY)

All HEP documents must contain ONLY current, accurate information. No
"superseded" markers, no strikethrough, no "see other doc" redirects for
removed content. The old content is deleted entirely.

- HEP-CORE-0007: Remove all remaining CHANNEL_NOTIFY_REQ, CHANNEL_BROADCAST_REQ,
  CHANNEL_EVENT_NOTIFY, CHANNEL_BROADCAST_NOTIFY references. Remove Peer-to-Peer
  section. Remove P2C socket framing specs. Remove Messenger callback docs.
  Replace with current BrokerRequestComm + HEP-0030 band protocol.
- HEP-CORE-0018: Remove all P2C establishment sequences (§15.3/15.4 P2C parts).
  Remove Messenger references. Remove ctrl_thread_ references. Update to
  reflect BrokerRequestComm + thread manager + band pub/sub.
- HEP-CORE-0011: Update script API section — remove old broadcast/send/consumers,
  document new band methods.
- Archive tech drafts that are fully implemented.

Each HEP must be a complete, self-consistent document after this pass.

---

## 7. Files Changed (Summary)

### New files
- `src/utils/ipc/band_registry.hpp`
- `src/utils/ipc/band_registry.cpp`
- `tests/test_layer3_datahub/test_datahub_band.cpp` (test driver)
- `tests/test_layer3_datahub/workers/datahub_band_workers.cpp`

### Modified files
- `src/utils/ipc/broker_service.cpp` — new dispatch + handlers
- `src/utils/CMakeLists.txt` — add band_registry.cpp
- `src/include/utils/broker_request_comm.hpp` — new band methods
- `src/utils/network_comm/broker_request_comm.cpp` — implementations
- `src/include/utils/role_api_base.hpp` — new band API
- `src/utils/service/role_api_base.cpp` — implementations + notification wiring
- `src/producer/producer_api.cpp` — Python bindings
- `src/consumer/consumer_api.cpp` — Python bindings
- `src/processor/processor_api.cpp` — Python bindings
- `src/scripting/lua_engine.cpp` — Lua band bindings
- `src/scripting/native_engine.cpp` — Native band bindings
- `tests/test_layer3_datahub/CMakeLists.txt` — new test files
