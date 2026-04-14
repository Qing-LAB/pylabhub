# HEP-CORE-0030: Band Messaging Protocol

| Property      | Value                                                |
|---------------|------------------------------------------------------|
| **HEP**       | `HEP-CORE-0030`                                      |
| **Title**     | Band Messaging Protocol                              |
| **Status**    | Draft                                                |
| **Created**   | 2026-04-10                                           |
| **Area**      | Control Plane Protocol                               |
| **Depends on**| HEP-CORE-0007 (Protocol Reference)                   |
| **Supersedes**| HEP-CORE-0007 §12 Peer-to-Peer category, CHANNEL_NOTIFY_REQ, CHANNEL_BROADCAST_REQ, CHANNEL_EVENT_NOTIFY, CHANNEL_BROADCAST_NOTIFY (old wire names) |

---

## 1. Abstract

This HEP defines **bands** as broker-hosted pub/sub messaging groups.
A band is a named facility where roles subscribe, exchange JSON messages,
and receive join/leave notifications. The broker maintains the member list
and performs fan-out delivery.

This protocol replaces:
- The old `CHANNEL_NOTIFY_REQ` / `CHANNEL_BROADCAST_REQ` asymmetric relay
- The old Peer-to-Peer category (HELLO/BYE, P2C direct sockets)
- The old `ChannelHandle` / `ChannelPattern` P2C socket infrastructure

---

## 2. Design Principles

1. **No ownership.** Any role can create a band by joining it. No
   producer/consumer asymmetry.
2. **Broker-mediated.** All messages flow through the broker's ROUTER
   socket. No direct P2C sockets between roles.
3. **JSON format.** All band message bodies are JSON.
4. **Symmetric membership.** All members are equal. Any member can send.
   All members receive.
5. **Auto-lifecycle.** Band is created on first join, deleted when
   last member leaves or times out.

---

## 3. Band Naming

Format: `#band_name@hub_uid`

- `#` prefix distinguishes band names from role UIDs and data registrations
- `band_name` is application-defined (alphanumeric + underscore)
- `@hub_uid` identifies which broker hub the band belongs to
- Examples: `#sensor_sync@HUB-TestLab-3F2A1B0E`, `#pipeline_ctrl@HUB-Prod-A1B2C3D4`

---

## 4. Broker Data Structure

The broker maintains a `BandRegistry` separate from the existing
`ChannelRegistry` (which handles data plane registration / REG_REQ).

```cpp
struct BandMember
{
    std::string role_uid;       // e.g. "PROD-Sensor-A1B2C3D4"
    std::string role_name;      // e.g. "sensor_producer"
    std::string zmq_identity;   // ROUTER identity for push notifications
    std::chrono::steady_clock::time_point joined_at;
};

struct Band
{
    std::string name;           // "#band_name@hub"
    std::vector<BandMember> members;
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

#### BAND_JOIN_REQ / BAND_JOIN_ACK

```
Direction:  Role → Broker → Role
Purpose:    Join a band (auto-creates if it doesn't exist)

Request payload:
  band                  string   "#band_name@hub"
  role_uid              string   Joining role's UID
  role_name             string   Joining role's display name

Reply payload (BAND_JOIN_ACK):
  status                string   "success"
  band                  string   "#band_name@hub"
  members               array    Current member list [{role_uid, role_name}, ...]

Broker behavior:
  1. If band doesn't exist → create it
  2. Add member to list (idempotent: if already member, no-op)
  3. Send BAND_JOIN_NOTIFY to all existing members (before adding)
  4. Reply with current member list (including new member)
```

#### BAND_LEAVE_REQ / BAND_LEAVE_ACK

```
Direction:  Role → Broker → Role
Purpose:    Leave a band

Request payload:
  band                  string   "#band_name@hub"
  role_uid              string   Leaving role's UID

Reply payload (BAND_LEAVE_ACK):
  status                string   "success"

Broker behavior:
  1. Remove member from list
  2. Send BAND_LEAVE_NOTIFY to remaining members
  3. If band is empty → delete it
  4. Reply BAND_LEAVE_ACK
```

#### BAND_MEMBERS_REQ / BAND_MEMBERS_ACK

```
Direction:  Role → Broker → Role
Purpose:    Query current band member list

Request payload:
  band                  string   "#band_name@hub"

Reply payload (BAND_MEMBERS_ACK):
  band                  string   "#band_name@hub"
  members               array    [{role_uid, role_name}, ...]
                                  Empty array if band doesn't exist
```

### 5.2 Fire-and-Forget Messages

#### BAND_BROADCAST_REQ

```
Direction:  Role → Broker (no reply)
Purpose:    Send JSON message to all band members

Payload:
  band                  string   "#band_name@hub"
  sender_uid            string   Sender's role UID
  body                  object   Application-defined JSON body

Broker behavior:
  1. Look up band in BandRegistry
  2. For each member (excluding sender):
       send_to_identity(socket, member.zmq_identity,
           "BAND_BROADCAST_NOTIFY", {band, sender_uid, body})
  3. If band doesn't exist → silently drop (no error)
```

### 5.3 Broker-Initiated Notifications

#### BAND_JOIN_NOTIFY

```
Direction:  Broker → All existing band members
Trigger:    Another role joined the band

Payload:
  band                  string   "#band_name@hub"
  role_uid              string   Joining role's UID
  role_name             string   Joining role's display name
```

#### BAND_LEAVE_NOTIFY

```
Direction:  Broker → All remaining band members
Trigger:    A role left the band (voluntarily or heartbeat timeout)

Payload:
  band                  string   "#band_name@hub"
  role_uid              string   Leaving role's UID
  reason                string   "voluntary" | "heartbeat_timeout"
```

#### BAND_BROADCAST_NOTIFY

```
Direction:  Broker → All band members (except sender)
Trigger:    BAND_BROADCAST_REQ received from a member

Payload:
  band                  string   "#band_name@hub"
  sender_uid            string   Sending role's UID
  body                  object   Application-defined JSON body (passthrough)
```

---

## 6. Heartbeat-Based Auto-Leave

When the broker detects a role's heartbeat has expired (existing liveness
mechanism in `check_heartbeat_timeouts()`):

1. Remove the role from ALL bands it belongs to
2. For each affected band, send `BAND_LEAVE_NOTIFY` with
   `reason: "heartbeat_timeout"` to remaining members
3. If any band becomes empty, delete it

This reuses the existing heartbeat infrastructure — no new liveness
mechanism needed.

---

## 7. Role-Side API

### 7.1 BrokerRequestComm Methods

```cpp
std::optional<nlohmann::json> band_join(const std::string &band,
                                         int timeout_ms = 5000);
bool band_leave(const std::string &band, int timeout_ms = 5000);
void band_broadcast(const std::string &band, const nlohmann::json &body);
std::optional<nlohmann::json> band_members(const std::string &band,
                                            int timeout_ms = 5000);
```

### 7.2 Script API

```python
# Join/leave
members = api.band_join("#sensor_sync")
api.band_leave("#sensor_sync")

# Messaging
api.band_broadcast("#sensor_sync", {"event": "calibration_done", "ts": 123.4})

# Query
members = api.band_members("#sensor_sync")
# Returns: [{"role_uid": "PROD-Sensor-A1B2", "role_name": "sensor_prod"}, ...]
```

### 7.3 Notification Delivery to Scripts

Band notifications arrive in the `msgs` parameter of script callbacks:

```python
def on_produce(slot, fz, msgs):
    for msg in msgs:
        if msg.event == "BAND_JOIN_NOTIFY":
            print(f"Member joined: {msg.details['role_uid']}")
        elif msg.event == "BAND_BROADCAST_NOTIFY":
            body = msg.details["body"]
            print(f"Band msg from {msg.details['sender_uid']}: {body}")
```

---

## 8. Message Body Conventions

### 8.1 Intended Usage

Band traffic is **informational / coordination** — group-wide signals to
coordinate actions and timing. It is expected to be **infrequent** relative
to data-plane traffic. Examples: "calibration_done", "start_run",
"pipeline_stage_ready".

Schema-controlled, high-frequency, or point-to-point data exchange between
roles **MUST NOT** use bands. Use the **inbox** (HEP-CORE-0027) for that:
it provides schema enforcement, per-sender sequence numbers, and direct P2P
delivery without broker fan-out.

### 8.2 Target-Field Convention (client-side filtering)

All band messages are fan-out broadcasts — every member receives every
message. There is no P2P band send. When a sender wants to direct a
message at a specific role, it includes a `target` (or `receiver`) field in
the JSON body and **all members agree on a filter rule**:

```json
{
  "target": "broadcast",         // or absent → all members act
  "event": "start_run",
  "params": {"duration_s": 60}
}
```

```json
{
  "target": "PROD-Sensor-A1B2",  // specific role uid → only that role acts
  "event": "recalibrate"
}
```

**Filter rule (applied by each receiving script):**

- `target` absent OR `target == "broadcast"` → every receiving member acts
- `target == <my role_uid>` OR `target == <my role_name>` → only that role acts
- Otherwise → ignore

The broker **does not enforce or inspect** the target field. This is a
purely client-side convention. The benefit: no separate P2P band API
is needed; coordination and targeted-coordination use the same primitive.

`target` / `receiver` are both acceptable names — pick one per project and
be consistent. The protocol reserves no other field names in the body;
applications define their own schema.

#### Best practice for targeted broadcasts (race semantics)

A targeted broadcast is **fire-and-forget** — the broker fans the JSON out
to every current member as of the moment the broadcast is processed, then
moves on. The intended recipient may have left the band between when the
sender last queried `BAND_MEMBERS_REQ` and when its broadcast arrives at
the broker. In that case, no member with the matching `target` will act,
and the sender receives no error or notification.

Implications:

- **Don't rely on band broadcast for delivery-guaranteed messages.** Use
  the inbox (HEP-CORE-0027) when you need acked, sequenced, schema-typed
  delivery to a known peer.
- **Don't poll `BAND_MEMBERS_REQ` to "verify" a target before sending.**
  The result is stale by the time it arrives. Just send and accept that
  some targeted broadcasts are missed.
- **The broker's role is to keep band membership correct, not to enforce
  delivery.** Membership is updated atomically when a role exits — for any
  reason (voluntary leave, heartbeat-death, deregister) — via the
  `on_channel_closed` / `on_consumer_closed` cleanup hook described in
  HEP-CORE-0023 §2.5. So if a targeted broadcast arrives at the broker
  *after* the target's exit was processed, the target won't be in the
  member set and the broker won't even attempt delivery to it.

Use band broadcasts for: status updates, control signals where loss is
tolerable, presence announcements, "anyone interested" notifications.

---

## 9. Superseded Protocol Elements

The following elements from HEP-CORE-0007 are superseded by this HEP:

| Old | Status | Replacement |
|-----|--------|-------------|
| §12.2 "Peer-to-Peer" message category | **SUPERSEDED** | Eliminated — no direct P2C sockets |
| CHANNEL_NOTIFY_REQ (§12.4) | **SUPERSEDED** | BAND_BROADCAST_REQ |
| CHANNEL_BROADCAST_REQ (§12.4) | **SUPERSEDED** | BAND_BROADCAST_REQ |
| CHANNEL_EVENT_NOTIFY (§12.5) | **SUPERSEDED** | BAND_BROADCAST_NOTIFY |
| CHANNEL_BROADCAST_NOTIFY (§12.5) | **SUPERSEDED** | BAND_BROADCAST_NOTIFY |
| HELLO / BYE peer protocol (§12.2) | **SUPERSEDED** | BAND_JOIN/LEAVE_NOTIFY |
| ChannelHandle (P2C socket RAII) | **SUPERSEDED** | Eliminated |
| ChannelPattern (PubSub/Pipeline/Bidir) | **SUPERSEDED** | Eliminated — broker does fan-out |

---

## 10. Relationship to Other Systems

| System | Relationship |
|--------|-------------|
| **Data plane** (QueueReader/QueueWriter, SHM, ZMQ PUSH/PULL) | Independent. Band messaging is for coordination, not data streaming. |
| **Broker registration** (REG_REQ, DISC_REQ) | Independent. Data plane registration uses different registry (ChannelRegistry). Band pub/sub uses BandRegistry. |
| **Inbox** (InboxQueue/InboxClient) | Complementary. Inbox is point-to-point. Band is pub/sub. `api.send_to(uid, data)` uses inbox. |
| **Heartbeat** (HEARTBEAT_REQ) | Reused. Broker heartbeat liveness drives auto-leave from bands. |
| **BrokerRequestComm** | Transport. Band API methods are added to BrokerRequestComm. Messages flow through its DEALER socket. |
