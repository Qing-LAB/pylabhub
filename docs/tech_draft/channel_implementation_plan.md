# Implementation Plan: Channel Pub/Sub Messaging

**Status**: Plan (2026-04-10)
**Prereq**: `channel_redesign.md` (design rationale)
**Branch**: `feature/lua-role-support`

---

## 1. Overview

Implement the channel as a broker-hosted pub/sub messaging group. This
involves changes to three layers:

1. **Broker (BrokerService)**: New registry, new message handlers, fan-out
2. **Role client (BrokerRequestChannel)**: New API methods
3. **Role framework (RoleAPIBase)**: Script-visible API, notification routing

No new ZMQ sockets. All channel messages flow through the existing broker
DEALER socket that `BrokerRequestChannel` already owns.

---

## 2. Broker Side

### 2.1 New data structure: ChannelGroupRegistry

File: `src/utils/ipc/channel_group_registry.hpp` (new, broker-internal)

```cpp
namespace pylabhub::broker
{

struct ChannelMember
{
    std::string role_uid;
    std::string role_name;
    std::string zmq_identity;   // ROUTER identity for push notifications
    std::chrono::steady_clock::time_point joined_at;
};

struct ChannelGroup
{
    std::string name;           // "#channel_name@hub"
    std::vector<ChannelMember> members;
    std::chrono::steady_clock::time_point created_at;
};

/// Separate from ChannelRegistry (which is about data plane registration).
/// This registry manages lightweight pub/sub messaging groups.
class ChannelGroupRegistry
{
public:
    /// Join a channel. Creates the channel if it doesn't exist.
    /// Returns the current member list (including the new member).
    nlohmann::json join(const std::string &channel,
                        const std::string &role_uid,
                        const std::string &role_name,
                        const std::string &zmq_identity);

    /// Leave a channel. Removes the member. Deletes channel if empty.
    /// Returns true if the member was found and removed.
    bool leave(const std::string &channel, const std::string &role_uid);

    /// Remove a member from ALL channels (called on heartbeat timeout).
    /// Returns list of {channel, remaining_members} for notification.
    std::vector<std::pair<std::string, std::string>>
    remove_from_all(const std::string &role_uid);

    /// Get member list for a channel.
    std::optional<nlohmann::json> members(const std::string &channel) const;

    /// Get all members of a channel as zmq_identity strings (for fan-out).
    std::vector<std::string> member_identities(
        const std::string &channel,
        const std::string &exclude_uid = {}) const;

    /// Check if a channel exists.
    bool exists(const std::string &channel) const;

private:
    std::unordered_map<std::string, ChannelGroup> groups_;
};

} // namespace pylabhub::broker
```

No mutex — accessed only from the broker run() thread (same discipline as
existing ChannelRegistry).

### 2.2 New message handlers in BrokerServiceImpl

File: `src/utils/ipc/broker_service.cpp`

Add to `process_message()` dispatch:

```cpp
else if (msg_type == "CHANNEL_JOIN_REQ")
{
    auto resp = handle_channel_join_req(payload, identity);
    send_reply(socket, identity,
        resp.value("status","") == "success" ? "CHANNEL_JOIN_ACK" : "ERROR", resp);
}
else if (msg_type == "CHANNEL_LEAVE_REQ")
{
    auto resp = handle_channel_leave_req(payload);
    send_reply(socket, identity,
        resp.value("status","") == "success" ? "CHANNEL_LEAVE_ACK" : "ERROR", resp);
}
else if (msg_type == "CHANNEL_MSG_REQ")
{
    handle_channel_msg_req(socket, payload, identity);
    // fire-and-forget — no reply
}
else if (msg_type == "CHANNEL_MEMBERS_REQ")
{
    auto resp = handle_channel_members_req(payload);
    send_reply(socket, identity, "CHANNEL_MEMBERS_ACK", resp);
}
```

### 2.3 Handler implementations

**handle_channel_join_req:**

```
Input:  {"channel": "#name@hub", "role_uid": "...", "role_name": "..."}
Output: {"status": "success", "channel": "#name@hub", "members": [...]}

1. channel_groups_.join(channel, role_uid, role_name, identity_str)
2. Fan out CHANNEL_JOIN_NOTIFY to all existing members (before adding new member):
   For each member in channel (excluding the joiner):
     send_to_identity(socket, member.zmq_identity, "CHANNEL_JOIN_NOTIFY",
       {"channel": ..., "role_uid": joiner_uid, "role_name": joiner_name})
3. Return ACK with current member list (including new member)
```

**handle_channel_leave_req:**

```
Input:  {"channel": "#name@hub", "role_uid": "..."}
Output: {"status": "success"}

1. channel_groups_.leave(channel, role_uid)
2. Fan out CHANNEL_LEAVE_NOTIFY to remaining members:
   For each remaining member:
     send_to_identity(socket, member.zmq_identity, "CHANNEL_LEAVE_NOTIFY",
       {"channel": ..., "role_uid": leaver_uid, "reason": "voluntary"})
3. If channel is now empty, it's auto-deleted (no notification needed)
```

**handle_channel_msg_req:**

```
Input:  {"channel": "#name@hub", "sender_uid": "...", "body": {...}}

1. Get member identities excluding sender
2. For each member:
     send_to_identity(socket, member.zmq_identity, "CHANNEL_MSG_NOTIFY",
       {"channel": ..., "sender_uid": ..., "body": {...}})
3. No reply (fire-and-forget)
```

**handle_channel_members_req:**

```
Input:  {"channel": "#name@hub"}
Output: {"channel": ..., "members": [{"role_uid": ..., "role_name": ...}, ...]}

1. channel_groups_.members(channel)
2. Return member list (or empty array if channel doesn't exist)
```

### 2.4 Heartbeat-based auto-leave

In `check_heartbeat_timeouts()` (existing method), when a role times out:

```
1. Find all channels the role belongs to:
   auto affected = channel_groups_.remove_from_all(role_uid)
2. For each affected channel:
   Fan out CHANNEL_LEAVE_NOTIFY with reason "heartbeat_timeout"
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
| CHANNEL_JOIN_REQ | Role→Broker | channel, role_uid, role_name |
| CHANNEL_JOIN_ACK | Broker→Role | status, channel, members[] |
| CHANNEL_LEAVE_REQ | Role→Broker | channel, role_uid |
| CHANNEL_LEAVE_ACK | Broker→Role | status |
| CHANNEL_MSG_REQ | Role→Broker | channel, sender_uid, body{} |
| CHANNEL_MEMBERS_REQ | Role→Broker | channel |
| CHANNEL_MEMBERS_ACK | Broker→Role | channel, members[] |
| CHANNEL_JOIN_NOTIFY | Broker→Role | channel, role_uid, role_name |
| CHANNEL_LEAVE_NOTIFY | Broker→Role | channel, role_uid, reason |
| CHANNEL_MSG_NOTIFY | Broker→Role | channel, sender_uid, body{} |

---

## 3. Role Client Side (BrokerRequestChannel)

### 3.1 New API methods

File: `src/include/utils/broker_request_channel.hpp`

```cpp
// ── Channel pub/sub messaging ────────────────────────────────────────

/// Join a channel (auto-creates if needed). Returns member list.
[[nodiscard]] std::optional<nlohmann::json>
join_channel(const std::string &channel, int timeout_ms = 5000);

/// Leave a channel.
bool leave_channel(const std::string &channel, int timeout_ms = 5000);

/// Send JSON message to all channel members (fire-and-forget).
void send_channel_msg(const std::string &channel,
                      const nlohmann::json &body);

/// Query current member list.
[[nodiscard]] std::optional<nlohmann::json>
query_channel_members(const std::string &channel, int timeout_ms = 5000);
```

### 3.2 Implementation

File: `src/utils/network_comm/broker_request_channel.cpp`

```cpp
std::optional<nlohmann::json>
BrokerRequestChannel::join_channel(const std::string &channel, int timeout_ms)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    payload["role_uid"] = /* from config or set_uid */;
    payload["role_name"] = /* from config or set_name */;
    return pImpl->do_request("CHANNEL_JOIN_REQ", "CHANNEL_JOIN_ACK",
                             std::move(payload), timeout_ms);
}

bool BrokerRequestChannel::leave_channel(const std::string &channel, int timeout_ms)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    payload["role_uid"] = /* from config */;
    auto result = pImpl->do_request("CHANNEL_LEAVE_REQ", "CHANNEL_LEAVE_ACK",
                                    std::move(payload), timeout_ms);
    return result.has_value() && result->value("status", "") == "success";
}

void BrokerRequestChannel::send_channel_msg(const std::string &channel,
                                             const nlohmann::json &body)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    payload["sender_uid"] = /* from config */;
    payload["body"] = body;
    pImpl->cmd_queue.push(SendCmd{"CHANNEL_MSG_REQ", std::move(payload)});
}

std::optional<nlohmann::json>
BrokerRequestChannel::query_channel_members(const std::string &channel,
                                             int timeout_ms)
{
    nlohmann::json payload;
    payload["channel"] = channel;
    return pImpl->do_request("CHANNEL_MEMBERS_REQ", "CHANNEL_MEMBERS_ACK",
                             std::move(payload), timeout_ms);
}
```

Note: `role_uid` and `role_name` need to be available to BrokerRequestChannel.
Options:
- (a) Store them in BrokerRequestChannel::Config (set during connect)
- (b) Pass them as parameters to each method
- (c) BrokerRequestChannel holds a reference to RoleAPIBase identity

Option (a) is cleanest — add `role_uid` and `role_name` to Config.

### 3.3 Notification routing

Channel notifications (CHANNEL_JOIN_NOTIFY, CHANNEL_LEAVE_NOTIFY,
CHANNEL_MSG_NOTIFY) arrive on the broker DEALER socket and are dispatched
by `recv_and_dispatch()` → `on_notification_cb`. RoleAPIBase must wire
this callback to route notifications to `core_.enqueue_message()`.

This is the existing `on_notification()` callback that was identified as
MISSING-002 in the code review — it needs to be wired now.

---

## 4. Role Framework (RoleAPIBase)

### 4.1 Script-visible API

File: `src/include/utils/role_api_base.hpp`

```cpp
// ── Channel pub/sub ─────────────────────────────────────────────────

/// Join a named channel. Auto-creates if it doesn't exist.
/// Returns member list JSON, or nullopt on failure.
[[nodiscard]] std::optional<nlohmann::json>
join_channel(const std::string &channel);

/// Leave a channel.
bool leave_channel(const std::string &channel);

/// Send JSON message to all channel members.
void send_channel_msg(const std::string &channel,
                      const nlohmann::json &body);

/// Query channel member list.
[[nodiscard]] std::optional<nlohmann::json>
channel_members(const std::string &channel);
```

Implementation forwards to `broker_channel_->join_channel()` etc.

### 4.2 Notification wiring

In `start_broker_thread()` (or a new `wire_broker_notifications()` method),
wire `broker_channel_->on_notification()` to dispatch channel notifications:

```cpp
broker_channel_->on_notification([core](const std::string &type,
                                         const nlohmann::json &body) {
    IncomingMessage msg;
    msg.event = type;  // "CHANNEL_JOIN_NOTIFY", "CHANNEL_MSG_NOTIFY", etc.
    msg.details = body;
    core->enqueue_message(std::move(msg));
});
```

Scripts receive these as entries in the `msgs` parameter of their callback.

### 4.3 Python/Lua/Native API registration

Each API class (ProducerAPI, ConsumerAPI, ProcessorAPI) registers:

```python
api.join_channel("#sensor_sync")
api.leave_channel("#sensor_sync")
api.send_channel_msg("#sensor_sync", {"event": "calibration_done"})
members = api.channel_members("#sensor_sync")
```

---

## 5. Thread Model

No new threads. All channel communication uses the existing broker thread:

```
Data loop thread:
    script calls api.send_channel_msg("#ch", body)
    → RoleAPIBase::send_channel_msg()
    → broker_channel_->send_channel_msg()  (enqueues SendCmd)
    → MonitoredQueue push → signal socket wakes broker thread

Broker thread (BrokerRequestChannel poll loop):
    drain command queue → send CHANNEL_MSG_REQ on DEALER
    poll DEALER → recv CHANNEL_MSG_NOTIFY from broker
    → on_notification_cb → core_.enqueue_message()

Data loop thread (next cycle):
    msgs = core_.drain_messages()
    → script receives {"event": "CHANNEL_MSG_NOTIFY", "details": {...}}
```

---

## 6. Implementation Order

### Step 1: ChannelGroupRegistry (broker-internal)
- New file: `src/utils/ipc/channel_group_registry.hpp`
- New file: `src/utils/ipc/channel_group_registry.cpp`
- Add to CMakeLists.txt
- Unit tests: L3 (join/leave/msg through real broker)

### Step 2: Broker message handlers
- Add dispatch entries in `process_message()`
- Implement 4 handlers in broker_service.cpp
- Add heartbeat auto-leave in `check_heartbeat_timeouts()`
- Test: L3 integration (role sends JOIN/LEAVE/MSG, verify fan-out)

### Step 3: BrokerRequestChannel API
- Add 4 methods to header + implementation
- Add `role_uid`/`role_name` to Config
- Test: L3 (BrokerRequestChannel → BrokerService round-trip)

### Step 4: RoleAPIBase + notification wiring
- Add channel methods to RoleAPIBase
- Wire `on_notification()` callback in `start_broker_thread()`
- Test: L3 (two roles, one sends msg, other receives notification)

### Step 5: Script API
- Register channel methods in ProducerAPI/ConsumerAPI/ProcessorAPI
- Lua engine: add channel functions
- Native engine: add channel function pointers
- Test: L2 engine tests, L4 integration

### Step 6: Migrate old channel usage
- `api.broadcast_channel()` → `api.send_channel_msg()`
- `api.notify_channel()` → `api.send_channel_msg()` with event field
- Remove old Messenger channel_notify/channel_broadcast methods
- Update HEP-CORE-0007, HEP-CORE-0018

### Step 7: Remove old P2C infrastructure
- Remove `start_comm_thread()` from RoleAPIBase
- Remove ChannelHandle, ChannelPattern
- Remove P2C socket creation from Messenger
- Remove Producer/Consumer P2C threads
- Remove `handle_*_events_nowait()` methods
- Update design docs, archive tech drafts

---

## 7. Files Changed (Summary)

### New files
- `src/utils/ipc/channel_group_registry.hpp`
- `src/utils/ipc/channel_group_registry.cpp`
- `tests/test_layer3_datahub/test_datahub_channel_group.cpp` (test driver)
- `tests/test_layer3_datahub/workers/datahub_channel_group_workers.cpp`

### Modified files
- `src/utils/ipc/broker_service.cpp` — new dispatch + handlers
- `src/utils/CMakeLists.txt` — add channel_group_registry.cpp
- `src/include/utils/broker_request_channel.hpp` — new channel methods
- `src/utils/network_comm/broker_request_channel.cpp` — implementations
- `src/include/utils/role_api_base.hpp` — new channel API
- `src/utils/service/role_api_base.cpp` — implementations + notification wiring
- `src/producer/producer_api.cpp` — Python bindings
- `src/consumer/consumer_api.cpp` — Python bindings
- `src/processor/processor_api.cpp` — Python bindings
- `src/scripting/lua_engine.cpp` — Lua bindings
- `src/scripting/native_engine.cpp` — Native bindings
- `tests/test_layer3_datahub/CMakeLists.txt` — new test files
