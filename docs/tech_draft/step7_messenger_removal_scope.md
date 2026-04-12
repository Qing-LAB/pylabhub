# Step 7 Scope: Messenger / ChannelHandle Removal

**Status**: Investigation complete (read-only). No code changes yet.
**Date**: 2026-04-11
**Prerequisite**: Step 6 complete (commit `1e20b94`).

---

## 1. Class disambiguation

There is **only one** `hub::Messenger` class. No inner classes with the same
name elsewhere.

- `src/include/utils/messenger.hpp` / `src/utils/ipc/messenger.cpp` — the class
- `src/utils/ipc/messenger_internal.hpp` — private `MessengerImpl` shared with
  the protocol handler file
- `src/utils/ipc/messenger_protocol.cpp` — per-command-type handler impls
  (`ChannelNotifyCmd`, `RegisterCmd`, `RolePresenceCmd`, etc.), not a separate
  class.

References to "messenger" inside `hub_producer.cpp` / `hub_consumer.cpp` are
to the same `hub::Messenger*` instance, passed in via constructor.

---

## 2. Messenger API — dead vs. live

### DEAD (only callers were removed in Step 6; safe to delete)

| Method | Old caller (now deleted) |
|---|---|
| `enqueue_channel_notify` | `RoleAPIBase::notify_channel` |
| `enqueue_channel_broadcast` | `RoleAPIBase::broadcast_channel` |
| `list_channels` | `RoleAPIBase::list_channels` |
| `request_shm_info` | `RoleAPIBase::request_shm_info` |

### LIVE (still have callers that survived Step 6)

#### Connection & lifecycle
- `connect()` — all 3 role hosts
- `disconnect()` — all 3 role hosts
- `on_hub_dead(cb)` — all 3 role hosts

#### Channel lifecycle (P2C data-plane infrastructure)
- `create_channel()` — `hub_producer.cpp`
- `connect_channel()` — `hub_consumer.cpp`
- `unregister_channel()` — Producer/Consumer destructors

#### Per-channel callback registration
- `on_channel_closing()` — Producer/Consumer (CHANNEL_CLOSING_NOTIFY)
- `on_force_shutdown()` — Producer/Consumer (grace expiry)
- `on_consumer_died()` — Producer (CONSUMER_DIED_NOTIFY)
- `on_channel_error()` — Producer/Consumer (CHANNEL_ERROR/EVENT notifications)

#### Heartbeat & metrics
- `suppress_periodic_heartbeat()` — Producer
- `enqueue_heartbeat()` — Producer
- `enqueue_heartbeat(metrics)` — Producer
- `enqueue_metrics_report()` — internal zmq_thread path

#### Schema & config queries
- `query_channel_schema()` — `schema_registry.cpp`

#### Role discovery & presence
- `query_role_presence()` — `role_host_helpers.hpp`, `role_api_base.cpp:790`
- `query_role_info()` — `role_api_base.cpp:725` (inbox discovery)

#### Data transport endpoint updates
- `update_endpoint()` — `hub_producer.cpp` (HEP-CORE-0021 port-0 resolution)

#### Checksum error reporting
- `report_checksum_error()` — possible ShmQueue caller (verify)

---

## 3. BrokerRequestChannel gap analysis

### Already present in BRC (no work needed)

- `connect(Config)` / `disconnect()`
- `query_role_presence(uid, timeout)`
- `query_role_info(uid, timeout)`
- `register_channel(opts, timeout)`
- `discover_channel(channel, opts, timeout)`
- `register_consumer(opts, timeout)`
- `deregister_channel(channel, timeout)`
- `deregister_consumer(channel, timeout)`
- `list_channels(timeout)`
- `query_shm_info(channel, timeout)`
- `send_heartbeat(channel, metrics)`
- `send_metrics_report(channel, uid, metrics)`
- `send_checksum_error(report)`
- `send_endpoint_update(channel, key, endpoint)`
- `on_notification(cb)` — generic notification callback
- `on_hub_dead(cb)`
- `set_periodic_task(action, interval, iter)`

### MISSING in BRC — per-channel, per-event callbacks

Messenger has `on_channel_closing(channel, cb)` / `on_force_shutdown(channel, cb)` /
`on_consumer_died(channel, cb)` / `on_channel_error(channel, cb)` — a `map<channel,
map<event, cb>>` inside MessengerImpl.

BRC currently has only **one generic `on_notification(cb)`**. Producer/Consumer
wiring in `hub_producer.cpp:519–555` and `hub_consumer.cpp:516–528` uses the
per-channel form.

**Options:**
- **(a)** Add `on_channel_event(channel, event, cb)` to BRC and give it a
  per-channel map. Straightforward port of Messenger's dispatch logic.
- **(b)** Keep BRC's single `on_notification`, move the dispatch map into
  Producer/Consumer (they already know their own channel name). Less code in
  the transport layer. Cleaner.

Option **(b)** is preferable — it treats BRC as a pure transport and keeps
application-level dispatch in the application layer.

### MISSING in BRC — schema query

Schema query path: `SchemaStore::query_from_broker()` calls
`Messenger::query_channel_schema()`. Need to either:
- **(a)** Add `query_channel_schema(channel, timeout)` to BRC
- **(b)** Give `SchemaStore` a `BrokerRequestChannel*` and use the existing
  request machinery directly.

---

## 4. Role host wiring today

All three role hosts currently own **both** a Messenger (as a member, not
pointer — `out_messenger_`) and a `BrokerRequestChannel` (as `unique_ptr`).
This is the transition state.

| Host | Messenger member | BRC member |
|---|---|---|
| `producer_role_host.cpp` | `out_messenger_` (declared in .hpp:96) | `broker_channel_` unique_ptr in .cpp |
| `consumer_role_host.cpp` | `out_messenger_` (.hpp:93) | `broker_channel_` unique_ptr |
| `processor_role_host.cpp` | `out_messenger_` (.hpp:100) | `broker_channel_` unique_ptr |

Messenger is passed into `hub::Producer::create()`, `hub::Consumer::connect()`,
`hub::Processor::connect()` as a reference — the Producer/Consumer/Processor
objects themselves call `create_channel` / `connect_channel` etc. on it.

---

## 5. Broker-side handlers

### Delete (no live caller after Step 6)

- `handle_channel_notify_req` (broker_service.cpp:1693) — CHANNEL_NOTIFY_REQ
- `handle_channel_list_req` (line 685–691 dispatch) — CHANNEL_LIST_REQ

### Keep — admin-only

- `handle_channel_broadcast_req` (line 1740) — CHANNEL_BROADCAST_REQ
  - **Still called** by `BrokerService::request_broadcast_channel()` (admin
    internal queue, line 463)
  - Used from hubshell's `pylabhub.broadcast_channel()` Python binding
  - Hubshell is `if(FALSE)` today → nothing active calls this either
  - **Can we delete it?** Yes, if we commit to rebuilding hubshell's
    broadcast mechanism on top of the new BAND_BROADCAST_REQ flow when hubshell
    returns. This is a **design decision**, not a mechanical one.

### Keep unconditionally

All the control-plane handlers stay: REG_REQ, DISC_REQ, HEARTBEAT_REQ,
METRICS_REPORT_REQ, ROLE_PRESENCE_REQ, ROLE_INFO_REQ, ENDPOINT_UPDATE_REQ,
SHM_BLOCK_QUERY_REQ, SCHEMA_REQ, CHECKSUM_ERROR_REPORT.

Plus the new HEP-0030 handlers: BAND_JOIN_REQ, BAND_LEAVE_REQ,
BAND_BROADCAST_REQ, BAND_MEMBERS_REQ.

---

## 6. ChannelRegistry vs. BandRegistry

These are **orthogonal** — both stay.

| Registry | Purpose | Used by |
|---|---|---|
| `ChannelRegistry` (OLD name, but live) | Data-plane channel discovery: REG_REQ advertisement, producer/consumer linking, heartbeat tracking, channel status (PendingReady / Ready / Closing), inbox endpoint storage for role-discovery | REG_REQ, CONSUMER_REG_REQ, HEARTBEAT_REQ, DISC_REQ, ROLE_INFO_REQ, etc. |
| `BandRegistry` (NEW, HEP-0030) | Pub/sub member list for bands; lightweight | BAND_JOIN/LEAVE/BROADCAST/MEMBERS_REQ |

ChannelRegistry is not going away. The name is misleading ("channel" = data
channel, not pub/sub band) but changing it is out of scope.

---

## 7. L3 tests that will need updating

### `test_datahub_broker_protocol.cpp` — 24 tests

**Delete** (test deleted methods):
- `ListChannels_ViaMessenger_Empty` (L407)
- `ListChannels_ViaMessenger_OneChannel` (L416)
- `ListChannels_ViaMessenger_WithConsumer` (L443)
- `NotifyReq_DeliveredToProducerOnly` (L329)
- `NotifyReq_EventFieldCorrect` (L366)
- `NotifyReq_UnknownChannel_Silent` (L389)

**Rewrite or delete** depending on admin broadcast decision:
- `BroadcastReq_FansOutToProducer` (L192)
- `BroadcastReq_FansOutToConsumer` (L217)
- `BroadcastReq_FansOutToAll` (L246)
- `BroadcastReq_UnknownChannel_Silent` (L281)
- `BroadcastReq_FieldsMatchSpec` (L297)
- `AdminBroadcast_DeliveredViaInternalQueue` (L477)

**Keep** (test surviving functionality):
- Heartbeat tests
- Registration tests
- `ChecksumErrorReport_*` (3 tests)
- `ClosingNotify_*` (2 tests)
- ~16 tests total survive

### Other active L3 tests

Need a grep pass for direct `Messenger::` calls in L3 test files besides
`test_datahub_broker_protocol.cpp`.

---

## 8. Proposed removal order

**Prerequisite phases** (additive; must be done first, each phase stays
passing):

1. **Phase 7.1** — Add dispatch abstraction. Either add per-channel callbacks
   to BRC, or (preferred) route BRC's generic `on_notification` through a
   local dispatch map inside Producer / Consumer. Leave Messenger alone.
2. **Phase 7.2** — Wire Producer / Consumer to use BRC for channel lifecycle
   and callbacks. Messenger still runs in parallel; nothing breaks.
3. **Phase 7.3** — Wire `SchemaStore` / `RoleAPIBase` / `role_host_helpers` to
   use BRC for query_role_info / query_role_presence / query_channel_schema.
   Messenger still runs in parallel.

**Decision point after 7.3:** Messenger is fully shadowed by BRC but still
running alongside. All live paths go through BRC. Now Messenger is
demolition-only.

**Demolition phases:**

4. **Phase 7.4** — Role hosts stop constructing Messenger. Verify build + tests.
   Messenger class still in tree but unreferenced.
5. **Phase 7.5** — Delete Messenger public methods that are now dead
   (connect/disconnect/query_*/enqueue_* etc. — the whole class surface).
6. **Phase 7.6** — Delete `messenger.hpp`, `messenger.cpp`, `messenger_internal.hpp`,
   `messenger_protocol.cpp`. Remove from CMakeLists. Scrub includes in umbrella
   headers (`plh_datahub.hpp`, `plh_datahub_client.hpp`).
7. **Phase 7.7** — Delete broker handlers for dead message types
   (`handle_channel_notify_req`, `handle_channel_list_req`; decide on
   `handle_channel_broadcast_req`).
8. **Phase 7.8** — Update `test_datahub_broker_protocol.cpp` (delete/rewrite
   the 9 tests above).

**Deferred** (separate refactor, not part of Step 7):

9. **Phase 7.9** — `ChannelHandle` / `ChannelPattern` removal. This is a
   Producer/Consumer socket-ownership refactor and is NOT coupled to
   Messenger removal. Currently `ChannelHandle` is used only by
   Producer/Consumer internals to wrap the ZMQ sockets and their RAII. Can
   stay as-is. Revisit after Messenger is gone to see if it still pulls its
   weight as an abstraction.

---

## 9. Key design decisions needed before implementation

1. **Per-channel dispatch location**: BRC-level map, or local dispatch map
   inside Producer/Consumer? Recommendation: local (Option b in §3).
2. **Admin broadcast fate**: Delete `CHANNEL_BROADCAST_REQ` handler +
   `request_broadcast_channel()` entirely, or keep for hubshell's future
   reintroduction? Recommendation: **delete**. When hubshell returns, it can
   use `BAND_BROADCAST_REQ` directly (probably by joining admin bands as a
   pseudo-role with a dedicated BRC connection).
3. **SchemaStore transport**: give it a `BrokerRequestChannel*` directly, or
   add `query_channel_schema` to BRC? Recommendation: **pass BRC pointer** —
   the schema query is a single-call use site, no need to pollute BRC API.
4. **ChannelHandle fate**: Step 7 in parallel with Messenger, or defer?
   Recommendation: **defer** (Phase 7.9). Decouples risk.

---

## 10. Risk summary

| Phase | Risk | Why |
|---|---|---|
| 7.1 dispatch routing | Medium | Callback wiring is subtle |
| 7.2 Producer/Consumer → BRC | Medium | Touches hot path; channel lifecycle changes |
| 7.3 helpers → BRC | Low | Method-to-method swap |
| 7.4 role hosts drop Messenger member | Low | Mechanical |
| 7.5–7.6 delete Messenger | Low | By this point nothing references it |
| 7.7 delete broker handlers | Low-Medium | Admin broadcast decision |
| 7.8 test updates | Low | Mechanical; tests already identified |
| 7.9 ChannelHandle | HIGH | Deferred intentionally |
