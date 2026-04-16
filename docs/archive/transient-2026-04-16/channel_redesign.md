# Tech Draft: Band Redesign — Lightweight Pub/Sub Messaging

**Status**: Draft (2026-04-10)
**Branch**: `feature/lua-role-support`
**Replaces**: Old producer-owned asymmetric channel model

---

## 1. What a Band Is

A band is a **named pub/sub messaging group** hosted at the broker.

- Named: `#band_name@broker`
- Any role can create it (auto-created on first join if it doesn't exist)
- All members are equal — no ownership, no asymmetry
- Any member can send a JSON message to the band
- All members receive all messages sent to the band
- Join/leave events are broadcast to all members
- Any member can query the current member list
- The broker maintains the member list and does the fan-out

A band is a **coordination mechanism** for roles. It is NOT a data
transport. Data flows through the queue abstraction (SHM/ZMQ). Band
messages are lightweight JSON used for signaling, coordination, and
application-level communication between roles.

---

## 2. Differences from Old Design

| Aspect | Old | New |
|--------|-----|-----|
| Ownership | Producer owns the channel | No ownership |
| Creation | Producer only (REG_REQ) | Any role (auto-create on first join) |
| Membership | Asymmetric: 1 producer, N consumers | Symmetric: all members equal |
| Communication | XPUB→SUB (producer broadcasts) | Broker fan-out (any member → all members) |
| Socket topology | Separate P2C sockets (ROUTER/DEALER + XPUB/SUB) | No P2C sockets — all through broker DEALER |
| Message format | Binary frames on P2C sockets | JSON only |
| Join/leave | HELLO/BYE on P2C ctrl socket; only producer notified | Broker notifies ALL members |
| Membership query | No per-band member list API | Any member can query |
| Peer-dead | P2C socket timeout | Broker-side heartbeat timeout → auto-remove + notify |

---

## 3. Broker Protocol

### New message types

**Request-reply:**

| Request | Reply | Purpose |
|---------|-------|---------|
| BAND_JOIN_REQ | BAND_JOIN_ACK | Join a band (auto-creates if needed) |
| BAND_LEAVE_REQ | BAND_LEAVE_ACK | Leave a band |
| BAND_MEMBERS_REQ | BAND_MEMBERS_ACK | Query current member list |

**Fire-and-forget:**

| Message | Purpose |
|---------|---------|
| BAND_BROADCAST_REQ | Send JSON message to all band members |

**Broker-initiated (unsolicited):**

| Notification | Purpose |
|-------------|---------|
| BAND_JOIN_NOTIFY | A member joined the band |
| BAND_LEAVE_NOTIFY | A member left the band (voluntary or dead) |
| BAND_BROADCAST_NOTIFY | A message was sent to the band |

### BAND_JOIN_REQ payload

```json
{
    "band": "#sensor_sync@hub",
    "role_uid": "PROD-Sensor-A1B2C3D4",
    "role_name": "sensor_producer"
}
```

Broker behavior:
- If band does not exist → create it
- Add role to member list
- Reply BAND_JOIN_ACK with current member list
- Send BAND_JOIN_NOTIFY to all existing members

### BAND_LEAVE_REQ payload

```json
{
    "band": "#sensor_sync@hub",
    "role_uid": "PROD-Sensor-A1B2C3D4"
}
```

Broker behavior:
- Remove role from member list
- Reply BAND_LEAVE_ACK
- Send BAND_LEAVE_NOTIFY to remaining members
- If member list is empty → delete band

### BAND_BROADCAST_REQ payload

```json
{
    "band": "#sensor_sync@hub",
    "sender_uid": "PROD-Sensor-A1B2C3D4",
    "body": { ... }
}
```

Broker behavior:
- Fan out BAND_BROADCAST_NOTIFY to all members EXCEPT the sender
- No reply (fire-and-forget)

### BAND_BROADCAST_NOTIFY payload (broker → members)

```json
{
    "band": "#sensor_sync@hub",
    "sender_uid": "PROD-Sensor-A1B2C3D4",
    "body": { ... }
}
```

### BAND_MEMBERS_REQ / ACK

```json
// Request
{ "band": "#sensor_sync@hub" }

// Reply
{
    "band": "#sensor_sync@hub",
    "members": [
        { "role_uid": "PROD-Sensor-A1B2C3D4", "role_name": "sensor_producer" },
        { "role_uid": "CONS-Logger-7E8F9A0B", "role_name": "data_logger" }
    ]
}
```

### Liveness

The broker already tracks role liveness via heartbeat. When a role's
heartbeat expires, the broker removes it from all bands it belongs
to and sends BAND_LEAVE_NOTIFY (reason: "heartbeat_timeout") to
remaining members.

---

## 4. Role-Side API

No new module needed. `BrokerRequestComm` gains band methods:

```cpp
// Join a band (auto-creates if not exists). Returns member list.
std::optional<nlohmann::json> band_join(const std::string &band,
                                         int timeout_ms = 5000);

// Leave a band.
bool band_leave(const std::string &band, int timeout_ms = 5000);

// Send JSON message to all band members.
void band_broadcast(const std::string &band,
                    const nlohmann::json &body);

// Query current member list.
std::optional<nlohmann::json> band_members(const std::string &band,
                                            int timeout_ms = 5000);
```

Band notifications (join/leave/broadcast) arrive via the existing
`on_notification()` callback → `core_.enqueue_message()` → script
receives in `msgs` parameter.

Script API:
```python
api.band_join("#sensor_sync")
api.band_leave("#sensor_sync")
api.band_broadcast("#sensor_sync", {"event": "calibration_done", "timestamp": 123.4})
members = api.band_members("#sensor_sync")
```

---

## 5. What This Eliminates

- **P2C sockets (ROUTER/DEALER + XPUB/SUB)**: No longer needed for
  band communication. All messages go through the broker DEALER.
- **`role_communication_channel` module**: Not needed. `BrokerRequestComm`
  handles everything.
- **`start_comm_thread()`**: No longer needed. The broker thread handles
  band messages as part of its poll loop.
- **ChannelHandle**: The RAII socket wrapper becomes unnecessary for
  band communication.
- **ChannelPattern (PubSub/Pipeline/Bidir)**: No longer relevant —
  the broker does the fan-out, not ZMQ socket topology.
- **HELLO/BYE protocol**: Replaced by BAND_JOIN/LEAVE_NOTIFY.
- **Peer-dead detection via P2C timeout**: Replaced by broker heartbeat
  liveness.

---

## 6. Point-to-Point: api.send_to() Migrates to Inbox

The old `api.send_to(identity, data)` sent via the P2C ROUTER socket
(direct producer→consumer). With P2C sockets eliminated, point-to-point
messaging uses the **inbox facility** (InboxQueue/InboxClient), which is
already a ROUTER/DEALER point-to-point system.

Migration:
- Old: `api.send_to(zmq_identity_hex, binary_data)` — P2C ROUTER socket
- New: `api.send_to(role_uid, json_data)` — internally opens inbox
  connection to target via `open_inbox(uid)`, sends through InboxClient

Benefits:
- Role UID (`"CONS-Logger-7E8F9A0B"`) instead of ZMQ identity hex bytes
- Broker-discoverable (ROLE_INFO_REQ returns inbox endpoint)
- Already implemented and tested (InboxQueue/InboxClient)
- JSON format consistent with band messages

---

## 7. What Stays Separate

- **Data plane**: QueueReader/QueueWriter (SHM or ZMQ PUSH/PULL).
  Completely independent of bands.
- **Broker protocol**: Registration (REG_REQ), discovery (DISC_REQ),
  heartbeat — these are about data plane setup, not band messaging.
- **Inbox**: Point-to-point messaging (InboxQueue/InboxClient). For
  targeted one-to-one communication, not pub/sub.
- **`BrokerRequestComm`**: Gains band methods but its core
  function (broker protocol DEALER) is unchanged.

---

## 7. Migration Strategy

### Phase 1: Add band protocol to broker + BrokerRequestComm

1. Add band registry to BrokerService (separate from existing
   channel registry which is about data plane registration)
2. Implement BAND_JOIN/LEAVE/BROADCAST/MEMBERS request handlers
3. Implement broker fan-out for BAND_BROADCAST_NOTIFY
4. Implement heartbeat-based auto-leave
5. Add band methods to BrokerRequestComm
6. Wire notification dispatch to core_.enqueue_message()
7. Add script API methods

### Phase 2: Migrate existing channel usage

1. Old `api.broadcast_channel()` → `api.band_broadcast()`
2. Old `api.notify_channel()` → `api.band_broadcast()` with event field
3. Old producer HELLO/BYE tracking → band join/leave notifications
4. Old peer-dead detection → broker heartbeat liveness

### Phase 3: Remove old P2C infrastructure

1. Remove `start_comm_thread()` from RoleAPIBase
2. Remove ChannelHandle, ChannelPattern
3. Remove P2C socket creation from Messenger's create_channel/connect_channel
4. Remove Producer/Consumer internal P2C threads (peer_thread, data_thread)
5. Remove `handle_*_events_nowait()` methods

---

## 8. Open Questions

1. **Message ordering**: Broker fan-out is sequential (one ROUTER send
   per member). Is ordering within a band guaranteed? (Yes — single
   broker thread processes messages in FIFO order.)

2. **Message delivery**: Fire-and-forget means no delivery guarantee.
   If a member's DEALER buffer is full, the message is dropped by ZMQ.
   Is this acceptable for coordination messages? (Probably yes — same
   as current broadcast behavior.)

3. **Band naming**: `#band_name@broker` — should the broker
   enforce this format, or is it just a convention?

4. **Cross-hub bands**: With federation (HEP-CORE-0022), should
   bands span multiple hubs? (Deferred — federation is a separate
   topic.)

5. **Relationship to existing REG_REQ**: The old REG_REQ creates a
   "channel" for data plane registration. The new BAND_JOIN_REQ
   creates a band for messaging. These are different registries.
   Should we rename the old one to avoid confusion? (e.g., "data_link"
   or "stream_registration"?)
