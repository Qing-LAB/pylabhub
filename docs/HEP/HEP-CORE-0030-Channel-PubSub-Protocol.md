# HEP-CORE-0030: Channel Pub/Sub Messaging Protocol

| Property      | Value                                                |
|---------------|------------------------------------------------------|
| **HEP**       | `HEP-CORE-0030`                                      |
| **Title**     | Channel Pub/Sub Messaging Protocol                   |
| **Status**    | Draft                                                |
| **Created**   | 2026-04-10                                           |
| **Area**      | Control Plane Protocol                               |
| **Depends on**| HEP-CORE-0007 (Protocol Reference)                   |
| **Supersedes**| HEP-CORE-0007 §12 Peer-to-Peer category, CHANNEL_NOTIFY_REQ, CHANNEL_BROADCAST_REQ, CHANNEL_EVENT_NOTIFY, CHANNEL_BROADCAST_NOTIFY |

---

## 1. Abstract

This HEP defines **channels** as broker-hosted pub/sub messaging groups.
A channel is a named facility where roles subscribe, exchange JSON messages,
and receive join/leave notifications. The broker maintains the member list
and performs fan-out delivery.

This protocol replaces:
- The old `CHANNEL_NOTIFY_REQ` / `CHANNEL_BROADCAST_REQ` asymmetric relay
- The old Peer-to-Peer category (HELLO/BYE, P2C direct sockets)
- The old `ChannelHandle` / `ChannelPattern` P2C socket infrastructure

---

## 2. Design Principles

1. **No ownership.** Any role can create a channel by joining it. No
   producer/consumer asymmetry.
2. **Broker-mediated.** All messages flow through the broker's ROUTER
   socket. No direct P2C sockets between roles.
3. **JSON format.** All channel message bodies are JSON.
4. **Symmetric membership.** All members are equal. Any member can send.
   All members receive.
5. **Auto-lifecycle.** Channel is created on first join, deleted when
   last member leaves or times out.

---

## 3. Channel Naming

Format: `#channel_name@hub_uid`

- `#` prefix distinguishes channel names from role UIDs and data registrations
- `channel_name` is application-defined (alphanumeric + underscore)
- `@hub_uid` identifies which broker hub the channel belongs to
- Examples: `#sensor_sync@HUB-TestLab-3F2A1B0E`, `#pipeline_ctrl@HUB-Prod-A1B2C3D4`

---

## 4. Broker Data Structure

The broker maintains a `ChannelGroupRegistry` separate from the existing
`ChannelRegistry` (which handles data plane registration / REG_REQ).

```cpp
struct ChannelMember
{
    std::string role_uid;       // e.g. "PROD-Sensor-A1B2C3D4"
    std::string role_name;      // e.g. "sensor_producer"
    std::string zmq_identity;   // ROUTER identity for push notifications
    std::chrono::steady_clock::time_point joined_at;
};

struct ChannelGroup
{
    std::string name;           // "#channel_name@hub"
    std::vector<ChannelMember> members;
    std::chrono::steady_clock::time_point created_at;
};
```

Thread safety: accessed only from the broker run() thread (same discipline
as existing ChannelRegistry — no internal mutex needed).

---

## 5. Wire Protocol

All messages use the standard 4-frame ROUTER format:
```
[zmq_identity] ['C'] [msg_type_string] [json_body]
```

### 5.1 Request-Reply Messages

#### CHANNEL_JOIN_REQ / CHANNEL_JOIN_ACK

```
Direction:  Role → Broker → Role
Purpose:    Join a channel (auto-creates if it doesn't exist)

Request payload:
  channel               string   "#channel_name@hub"
  role_uid              string   Joining role's UID
  role_name             string   Joining role's display name

Reply payload (CHANNEL_JOIN_ACK):
  status                string   "success"
  channel               string   "#channel_name@hub"
  members               array    Current member list [{role_uid, role_name}, ...]

Broker behavior:
  1. If channel doesn't exist → create it
  2. Add member to list (idempotent: if already member, no-op)
  3. Send CHANNEL_JOIN_NOTIFY to all existing members (before adding)
  4. Reply with current member list (including new member)
```

#### CHANNEL_LEAVE_REQ / CHANNEL_LEAVE_ACK

```
Direction:  Role → Broker → Role
Purpose:    Leave a channel

Request payload:
  channel               string   "#channel_name@hub"
  role_uid              string   Leaving role's UID

Reply payload (CHANNEL_LEAVE_ACK):
  status                string   "success"

Broker behavior:
  1. Remove member from list
  2. Send CHANNEL_LEAVE_NOTIFY to remaining members
  3. If channel is empty → delete it
  4. Reply CHANNEL_LEAVE_ACK
```

#### CHANNEL_MEMBERS_REQ / CHANNEL_MEMBERS_ACK

```
Direction:  Role → Broker → Role
Purpose:    Query current channel member list

Request payload:
  channel               string   "#channel_name@hub"

Reply payload (CHANNEL_MEMBERS_ACK):
  channel               string   "#channel_name@hub"
  members               array    [{role_uid, role_name}, ...]
                                  Empty array if channel doesn't exist
```

### 5.2 Fire-and-Forget Messages

#### CHANNEL_MSG_REQ

```
Direction:  Role → Broker (no reply)
Purpose:    Send JSON message to all channel members

Payload:
  channel               string   "#channel_name@hub"
  sender_uid            string   Sender's role UID
  body                  object   Application-defined JSON body

Broker behavior:
  1. Look up channel in ChannelGroupRegistry
  2. For each member (excluding sender):
       send_to_identity(socket, member.zmq_identity,
           "CHANNEL_MSG_NOTIFY", {channel, sender_uid, body})
  3. If channel doesn't exist → silently drop (no error)
```

### 5.3 Broker-Initiated Notifications

#### CHANNEL_JOIN_NOTIFY

```
Direction:  Broker → All existing channel members
Trigger:    Another role joined the channel

Payload:
  channel               string   "#channel_name@hub"
  role_uid              string   Joining role's UID
  role_name             string   Joining role's display name
```

#### CHANNEL_LEAVE_NOTIFY

```
Direction:  Broker → All remaining channel members
Trigger:    A role left the channel (voluntarily or heartbeat timeout)

Payload:
  channel               string   "#channel_name@hub"
  role_uid              string   Leaving role's UID
  reason                string   "voluntary" | "heartbeat_timeout"
```

#### CHANNEL_MSG_NOTIFY

```
Direction:  Broker → All channel members (except sender)
Trigger:    CHANNEL_MSG_REQ received from a member

Payload:
  channel               string   "#channel_name@hub"
  sender_uid            string   Sending role's UID
  body                  object   Application-defined JSON body (passthrough)
```

---

## 6. Heartbeat-Based Auto-Leave

When the broker detects a role's heartbeat has expired (existing liveness
mechanism in `check_heartbeat_timeouts()`):

1. Remove the role from ALL channels it belongs to
2. For each affected channel, send `CHANNEL_LEAVE_NOTIFY` with
   `reason: "heartbeat_timeout"` to remaining members
3. If any channel becomes empty, delete it

This reuses the existing heartbeat infrastructure — no new liveness
mechanism needed.

---

## 7. Role-Side API

### 7.1 BrokerRequestChannel Methods

```cpp
std::optional<nlohmann::json> join_channel(const std::string &channel,
                                            int timeout_ms = 5000);
bool leave_channel(const std::string &channel, int timeout_ms = 5000);
void send_channel_msg(const std::string &channel, const nlohmann::json &body);
std::optional<nlohmann::json> query_channel_members(const std::string &channel,
                                                     int timeout_ms = 5000);
```

### 7.2 Script API

```python
# Join/leave
members = api.join_channel("#sensor_sync")
api.leave_channel("#sensor_sync")

# Messaging
api.send_channel_msg("#sensor_sync", {"event": "calibration_done", "ts": 123.4})

# Query
members = api.channel_members("#sensor_sync")
# Returns: [{"role_uid": "PROD-Sensor-A1B2", "role_name": "sensor_prod"}, ...]
```

### 7.3 Notification Delivery to Scripts

Channel notifications arrive in the `msgs` parameter of script callbacks:

```python
def on_produce(slot, fz, msgs):
    for msg in msgs:
        if msg.event == "CHANNEL_JOIN_NOTIFY":
            print(f"Member joined: {msg.details['role_uid']}")
        elif msg.event == "CHANNEL_MSG_NOTIFY":
            body = msg.details["body"]
            print(f"Channel msg from {msg.details['sender_uid']}: {body}")
```

---

## 8. Superseded Protocol Elements

The following elements from HEP-CORE-0007 are superseded by this HEP:

| Old | Status | Replacement |
|-----|--------|-------------|
| §12.2 "Peer-to-Peer" message category | **SUPERSEDED** | Eliminated — no direct P2C sockets |
| CHANNEL_NOTIFY_REQ (§12.4) | **SUPERSEDED** | CHANNEL_MSG_REQ |
| CHANNEL_BROADCAST_REQ (§12.4) | **SUPERSEDED** | CHANNEL_MSG_REQ |
| CHANNEL_EVENT_NOTIFY (§12.5) | **SUPERSEDED** | CHANNEL_MSG_NOTIFY |
| CHANNEL_BROADCAST_NOTIFY (§12.5) | **SUPERSEDED** | CHANNEL_MSG_NOTIFY |
| HELLO / BYE peer protocol (§12.2) | **SUPERSEDED** | CHANNEL_JOIN/LEAVE_NOTIFY |
| ChannelHandle (P2C socket RAII) | **SUPERSEDED** | Eliminated |
| ChannelPattern (PubSub/Pipeline/Bidir) | **SUPERSEDED** | Eliminated — broker does fan-out |

---

## 9. Relationship to Other Systems

| System | Relationship |
|--------|-------------|
| **Data plane** (QueueReader/QueueWriter, SHM, ZMQ PUSH/PULL) | Independent. Channel is for coordination messaging, not data streaming. |
| **Broker registration** (REG_REQ, DISC_REQ) | Independent. Data plane registration uses different registry (ChannelRegistry). Channel pub/sub uses ChannelGroupRegistry. |
| **Inbox** (InboxQueue/InboxClient) | Complementary. Inbox is point-to-point. Channel is pub/sub. `api.send_to(uid, data)` uses inbox. |
| **Heartbeat** (HEARTBEAT_REQ) | Reused. Broker heartbeat liveness drives auto-leave from channels. |
| **BrokerRequestChannel** | Transport. Channel API methods are added to BrokerRequestChannel. Messages flow through its DEALER socket. |
