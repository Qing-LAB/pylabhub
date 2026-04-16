# Obsolete Code & Replacement Map

**Status**: Plan (2026-04-10)
**Context**: Channel redesign + Messenger elimination

This document lists everything that becomes obsolete and its replacement.
NO backward compatibility. Clean cut.

---

## 1. Messenger Module — DELETE ENTIRELY

The entire Messenger module is replaced by `BrokerRequestComm`.

| File | Action |
|------|--------|
| `src/utils/ipc/messenger.cpp` | DELETE |
| `src/utils/ipc/messenger_protocol.cpp` | DELETE |
| `src/utils/ipc/messenger_internal.hpp` | DELETE |
| `src/include/utils/messenger.hpp` | DELETE |

**Replacement**: `BrokerRequestComm` (already implemented) handles all
broker protocol. Band messaging is new BAND_JOIN/LEAVE/BROADCAST protocol.

---

## 2. P2C Socket Infrastructure — DELETE ENTIRELY

Direct producer↔consumer ZMQ sockets are eliminated. Band messaging
goes through the broker. Point-to-point goes through inbox.

| File | Action |
|------|--------|
| `src/utils/ipc/channel_handle.cpp` | DELETE |
| `src/utils/ipc/channel_handle_factory.hpp` | DELETE |
| `src/include/utils/channel_handle.hpp` | DELETE |
| `src/include/utils/channel_pattern.hpp` | DELETE |
| `src/utils/hub/channel_handle_internals.hpp` | DELETE |

**Replacement**: No replacement. Band messaging goes through broker.
The P2C sockets carried:
- HELLO/BYE → replaced by BAND_JOIN/LEAVE_NOTIFY
- `api.broadcast(data)` → replaced by `api.band_broadcast(band, body)`
- `api.send(identity, data)` → replaced by inbox `api.send_to(role_uid, data)`
- Peer-dead → replaced by broker heartbeat liveness
- `api.consumers()` → replaced by `api.band_members(band)`

---

## 3. Broker Protocol Messages — OBSOLETE

These broker message types are replaced by the new channel protocol:

| Old message | Replacement |
|-------------|-------------|
| `CHANNEL_NOTIFY_REQ` | `BAND_BROADCAST_REQ` (send to all members) |
| `CHANNEL_BROADCAST_REQ` | `BAND_BROADCAST_REQ` (same — all members receive) |
| `CHANNEL_BROADCAST_NOTIFY` | `BAND_BROADCAST_NOTIFY` |
| `CHANNEL_EVENT_NOTIFY` | `BAND_BROADCAST_NOTIFY` with event field in body |

The handler code in `broker_service.cpp` for these old messages is deleted:
- `handle_channel_notify_req()` — DELETE
- `handle_channel_broadcast_req()` — DELETE
- `relay_notify_to_peers()` — DELETE (federation relay for old notify)

---

## 4. RoleAPIBase Methods — REMOVE AND REPLACE

| Old method | Action | Replacement |
|------------|--------|-------------|
| `set_messenger(Messenger *m)` | DELETE | Not needed — `BrokerRequestComm` handles broker protocol |
| `notify_channel(target, event, data)` | DELETE | `band_broadcast(band, body)` |
| `broadcast_channel(target, msg, data)` | DELETE | `band_broadcast(band, body)` |
| `broadcast(data, size)` | DELETE | `band_broadcast(band, body)` |
| `send(identity_hex, data, size)` | DELETE | `send_to(role_uid, data)` via inbox |
| `connected_consumers()` | DELETE | `band_members(band)` |
| `start_comm_thread()` | DELETE | Not needed — no P2C sockets to poll |
| `wire_event_callbacks()` | MODIFY | Remove Messenger/P2C callback wiring; keep broker notification wiring |

New methods (already in plan):
- `band_join(band)`
- `band_leave(band)`
- `band_broadcast(band, body)`
- `band_members(band)`

---

## 5. Script API — REMOVE AND REPLACE

### Python (all 3 API classes)

| Old | Action | New |
|-----|--------|-----|
| `api.broadcast(data)` | DELETE | `api.band_broadcast("#band", {"data": ...})` |
| `api.send(identity, data)` | DELETE | `api.send_to(role_uid, data)` (via inbox) |
| `api.consumers()` | DELETE | `api.band_members("#band")` |
| `api.notify_channel(target, event, data)` | DELETE | `api.band_broadcast("#band", {"event": ..., "data": ...})` |
| `api.broadcast_channel(target, msg, data)` | DELETE | `api.band_broadcast("#band", {"msg": ..., "data": ...})` |

New:
- `api.band_join("#band")` — returns member list
- `api.band_leave("#band")`
- `api.band_broadcast("#band", body_dict)` — JSON to all members
- `api.band_members("#band")` — list of {role_uid, role_name}

### Lua

Same changes. Old `api.broadcast()`, `api.send()`, `api.consumers()`,
`api.notify_channel()`, `api.broadcast_channel()` deleted. New band
functions added.

### Native

Same changes in `native_engine_api.h` — old function pointers removed,
new band function pointers added.

---

## 6. Producer/Consumer Internal P2C Code — REMOVE

### hub_producer.cpp

| What | Action |
|------|--------|
| `Messenger *messenger` member | DELETE |
| `ChannelHandle handle` member | DELETE |
| `peer_thread` (polls ROUTER) | DELETE |
| `handle_peer_events_nowait()` | DELETE |
| `peer_ctrl_socket_handle()` | DELETE |
| `recv_and_dispatch_ctrl_()` | DELETE |
| `consumer_identities` list | DELETE |
| `on_consumer_joined/left/message` callbacks | DELETE |
| `on_peer_dead` callback | DELETE |
| `send_to()`, `broadcast()` methods (P2C socket) | DELETE |
| `connected_consumers()` | DELETE |
| `Producer::create(Messenger &, opts)` factory | MODIFY — remove Messenger param |
| `Producer::start()` / `start_embedded()` | MODIFY — remove P2C thread logic |

### hub_consumer.cpp

| What | Action |
|------|--------|
| `Messenger *messenger` member | DELETE |
| `ChannelHandle handle` member | DELETE |
| `data_thread` (polls SUB) | DELETE |
| `ctrl_thread` (polls DEALER) | DELETE |
| `handle_data_events_nowait()` | DELETE |
| `handle_ctrl_events_nowait()` | DELETE |
| `data_zmq_socket_handle()` | DELETE |
| `ctrl_zmq_socket_handle()` | DELETE |
| `recv_and_dispatch_data_()` | DELETE |
| `recv_and_dispatch_ctrl_()` | DELETE |
| `on_zmq_data` callback | DELETE |
| `on_producer_message` callback | DELETE |
| `on_peer_dead` callback | DELETE |
| `Consumer::connect(Messenger &, opts)` factory | MODIFY — remove Messenger param |
| `Consumer::start()` / `start_embedded()` | MODIFY — remove P2C thread logic |

---

## 7. Role Host Changes

### All 3 role hosts

| What | Action |
|------|--------|
| `Messenger out_messenger_` / `in_messenger_` member | DELETE |
| `messenger.connect(broker, pubkey, ...)` call | DELETE (BrokerRequestComm handles this) |
| `messenger.suppress_periodic_heartbeat()` call | DELETE |
| `messenger.enqueue_heartbeat()` initial call | DELETE |
| `messenger.on_hub_dead(nullptr)` in teardown | DELETE |
| `wire_event_callbacks()` P2C/Messenger sections | MODIFY — remove |

---

## 8. Existing Test Code — UPDATE

Tests that rely on P2C sockets, Messenger, ChannelHandle, or old channel
APIs need updating:

| Test file | Action |
|-----------|--------|
| `test_datahub_hub_api.cpp` (ActiveProducerConsumerCallbacks) | REWRITE — uses P2C on_zmq_data |
| `test_datahub_broker_protocol.cpp` (channel_notify, broadcast tests) | REWRITE — use new CHANNEL_MSG |
| `test_datahub_channel.cpp` | REWRITE or DELETE — tests old channel model |
| `test_channel_broadcast.cpp` (L4) | REWRITE — use new channel API |
| Workers that use Messenger directly | UPDATE — use BrokerRequestComm |

---

## 9. Config/HEP Changes

### channel_access_policy.hpp — KEEP (orthogonal to channel messaging)

### HEP updates needed

| HEP | Section | Change | Status |
|-----|---------|--------|--------|
| HEP-CORE-0007 | §12.6 Peer-to-Peer | Replaced with REMOVED note | **DONE** (2026-04-11) |
| HEP-CORE-0007 | §12.7 Sequence diagram | HELLO (P2C) line removed | **DONE** (2026-04-11) |
| HEP-CORE-0007 | §12.8 Event table | consumer_joined/left/producer_message marked REMOVED | **DONE** (2026-04-11) |
| HEP-CORE-0007 | §12.9 Design notes | Rewritten for BrokerRequestComm dispatch | **DONE** (2026-04-11) |
| HEP-CORE-0007 | REG_REQ/DISC_ACK payloads | P2C fields removed (channel_pattern, zmq_ctrl/data_endpoint, zmq_pubkey) | **DONE** (2026-04-11) |
| HEP-CORE-0018 | §15.5 Control Plane | Rewritten: BrokerRequestComm, no P2C sockets | **DONE** (2026-04-11) |
| HEP-CORE-0018 | §15.8 Service operations | send_ctrl/send_to/connected_consumers removed | **DONE** (2026-04-11) |
| HEP-CORE-0018 | Thread diagrams | HELLO/BYE references removed | **DONE** (2026-04-11) |
| HEP-CORE-0002 | §6.8 hub::Producer/Consumer | peer_thread/ctrl_thread/HELLO/BYE removed | **DONE** (2026-04-11) |
| HEP-CORE-0002 | Peer messaging section | Replaced with REMOVED note | **DONE** (2026-04-11) |
| HEP-CORE-0002 | Frame types, source refs | Messenger → BrokerRequestComm | **DONE** (2026-04-11) |
| HEP-CORE-0007 | §10 Wire protocol | Add BAND_JOIN/LEAVE/BROADCAST/MEMBERS message specs | Pending |
| HEP-CORE-0011 | Script API | Update: new band methods, remove old broadcast/send/consumers | **DONE** (2026-04-11)

---

## 10. Implementation Order

1. **Add new** (band pub/sub): broker registry, protocol, BrokerRequestComm methods, RoleAPIBase API, script bindings
2. **Wire notifications**: on_notification callback routes band events to core_.enqueue_message()
3. **Add tests**: L3 band tests against real broker
4. **Migrate script API**: replace old methods with new in all 3 engines
5. **Remove P2C**: delete ChannelHandle, P2C sockets, comm thread, old callbacks
6. **Remove Messenger**: delete entire module
7. **Update Producer/Consumer**: remove Messenger param from factories, remove P2C threads
8. **Update tests**: rewrite affected tests
9. **Update HEPs**: document new protocol, deprecate old
