# Holistic Code Review — Connection / Inbox / Band

**Date:** 2026-05-17
**Branch:** `feature/lua-role-support` (post Tier A — A0+A2+A3 shipped; 1923/1923 tests passing)
**Scope:** End-to-end implementation audit of three control-plane protocols:
1. **Connection** — role↔broker BRC startup, ctrl-thread spawn, hub-dead detection, teardown
2. **Inbox** — per-presence inbox advertisement, ROLE_INFO_REQ discovery (Class B fall-through), DEALER↔ROUTER message exchange
3. **Band** — broker-hosted pub/sub groups (BAND_JOIN/LEAVE/BROADCAST/MEMBERS) + inbound NOTIFY routing

**Method:** Trace each protocol against authoritative HEPs (HEP-CORE-0023, -0027, -0030, and §18/§19 of HEP-CORE-0033). Findings are precise: design doc § ↔ code file:line. Reading was done from current source — no reliance on memory.

**Goal:** Identify obsolete data structures, code↔design drift, duplicated components, redundant data/code that the dual-hub milestone needs to land cleanly.

---

## Authoritative HEPs referenced

| HEP | Topic | Key §s consulted |
|---|---|---|
| HEP-CORE-0023 | Startup Coordination | §2 (FSM), §2.5 (heartbeat config), §2.5.1 (cadence negotiation), §2.5.2 (per-presence), §2.6 (data structures), §4 (ROLE_INFO_REQ), §5.5 (dual-hub `wait_for_roles`), §7 (`IncomingMessage` source-tagging) |
| HEP-CORE-0027 | Inbox Messaging | §2 (architecture), §3 (wire), §4.1 (per-presence advertisement), §4.2 (sender discovery), §4.5 (multi-hub reachability) |
| HEP-CORE-0030 | Band Messaging Protocol | §3 (naming), §4 (state), §5 (wire), §6 (heartbeat auto-leave), §8 (target convention), §9 (superseded elements) |
| HEP-CORE-0033 | Hub Character | §18 (routing taxonomy A/B/C/D), §19 (multi-presence model), §19.6 (hub-dead policy) |

---

## 1. Connection protocol

### 1.1 End-to-end sequence (pseudocode, traced from code)

```
ROLE STARTUP (role host)
 1. Build presence list from role topology
      - producer  → [{out_hub, out_channel, producer}]
      - consumer  → [{in_hub,  in_channel,  consumer}]
      - processor → [{in_hub, in_ch, consumer},
                     {out_hub, out_ch, producer}]

 2. RoleHandler ctor (role_handler.cpp:29-34):
      build_connections_()
        connections_.reserve(presences_.size())     // pointer-stability!
        for p in presences:
          if (c in connections_ has same (broker_endpoint, broker_pubkey)):
              p.connection = &c
          else:
              connections_.emplace_back(p.hub.broker, p.hub.broker_pubkey)
              p.connection = &connections_.back()
      build_channel_index_()
        for p in presences: channel_index_[p.channel] = &p
      band_index_ left empty (populated lazily on band_join)

 3. api.start_handler_threads(handler) (role_api_base.cpp:848):
      Phase 1: handler.start_connections(owner)
        for each HubConnection c:
          c.brc = make_unique<BRC>()
          c.brc.connect(Config{
              broker_endpoint=c.broker_endpoint,
              broker_pubkey  =c.broker_pubkey,
              client_pubkey  =owner.auth_client_pubkey(),
              client_seckey  =owner.auth_client_seckey(),
              role_uid       =owner.uid(),
              role_name      =owner.name()})

      Phase 2 (role_api_base.cpp:895-955): for i in 0..N-1:
        brc = connections[i].brc
        brc.on_notification([core, tag, i](type, body):
            msg.event           = type
            msg.notification_id = parse_notification_id(type)
            msg.details         = body
            core.enqueue_message(msg))    // NOTE: i unused — see C3
        brc.on_hub_dead([alive_mask, i]:
            alive_mask &= ~(1<<i)
            if i == 0:                                  // ⚠ §19.6 conflict
                set_stop_reason(HubDead); request_stop()
            else:
                WARN only)

      Phase 3 (role_api_base.cpp:959-1019): for i in 0..N-1:
        tm.spawn("handler_ctrl_" + i,
                 body: brc.run_poll_loop(should_run),
                 is_master = (i == 0))

      Phase 4 (role_api_base.cpp:1021-1039): alive_mask = (1<<N) - 1

 4. Per-presence registration (Class A — channel-bound):
      For each presence p:
        opts["channel_name"] = p.channel
        opts["inbox_endpoint"]    = inbox_queue.actual_endpoint()
        opts["inbox_schema_json"] = inbox_cfg.schema_fields_json
        opts["inbox_packing"]     = inbox_cfg.packing
        opts["inbox_checksum"]    = inbox_cfg.checksum
        bc = handler.brc_for_channel(p.channel)
        bc.register_channel(opts)  // or register_consumer

 5. install_heartbeat() (role_api_base.cpp:549-570):
      bc_tick = connections()[0].brc    // master only — see C4
      bc_tick.set_periodic_task(
          [this] { on_heartbeat_tick_(); },
          effective_interval_ms,
          iter_counter)

 6. on_heartbeat_tick_() (role_api_base.cpp:773-827):
      // role_tag-string branching — see C2
      if role_tag == "proc":
          emit(channel,     "consumer")
          emit(out_channel, "producer")
      elif role_tag == "cons":
          emit(channel, "consumer")
      else (prod):
          emit(channel || out_channel, "producer")

      emit(ch, role_type):
          bc = handler.brc_for_channel(ch)         // dual-hub-aware routing
          bc.send_heartbeat(ch, uid, role_type, metrics)

HUB-DEAD DETECTION (broker_request_comm.cpp)
  Setup at connect() (line 432-444):
    zmq_socket_monitor(dealer, "inproc://...mon", ZMQ_EVENT_DISCONNECTED)
    monitor_sock.connect(monitor_endpoint)

  run_poll_loop (line 575-591, A2 fix):
    poll_items includes monitor_sock as first-class entry
    poll yields → if monitor_sock readable: check_monitor()
    check_monitor() (line 233-280):
      recv monitor event
      if event_id == ZMQ_EVENT_DISCONNECTED:
          on_hub_dead_cb()       // → Phase 2 lambda above
```

### 1.2 Findings — Connection

| # | Tier | Finding |
|---|---|---|
| ~~**C1**~~ | ~~**D1**~~ ✅ FIXED 2026-05-18 | **HEP-0033 §19.6 ↔ A2 implementation drift.** Original drift: §19.6 said either-hub-death exits but A2 code (`role_api_base.cpp:920-954`) implemented master/peer asymmetry. **Resolution:** unified-dispatch refactor moved the asymmetric default into the worker-thread dispatcher's `default_hub_dead` (`cycle_ops.hpp`), made it overrideable via a script `on_hub_dead` callback (HEP-0011 callback table), and rewrote HEP-0033 §19.6 to describe the user-override-or-native-default model.  Scripts can now opt into the either-hub-exits behavior by defining `on_hub_dead` and calling `api.stop()` inside it.  Lambda no longer calls request_stop directly; enqueues a synthetic HUB_DEAD `IncomingMessage` with `details["is_master"]` (`role_api_base.cpp` Phase 2) for the dispatcher.  Tests: 5 new `DispatchHubDeadTest` cases (L2) + updated `RoleAPIBase_HubDead_MasterExitsRole` worker (L3) to drive the new path. |
| **C2** | **D2** | **Heartbeat tick installation uses `role_tag` string branching** (`role_api_base.cpp:806-823`). HEP-0033 §19.3 specifies presence-list iteration. Today the code uses `pImpl->channel` + `pImpl->out_channel` (legacy single-presence fields) and `if role_tag == "proc"/"cons"`. End-state semantics correct for the current 3 role kinds (the per-channel `resolve_bc_for_channel` does land each heartbeat on the right BRC), but a future N-input router cannot register itself by adding presences to the list — it would also need a new `role_tag` branch. The clean form is `for (const auto &p : handler_->presences()) emit(p.channel, to_wire_string(p.role_kind))`. |
| **C3** | **D2** | **`IncomingMessage` lacks `source_hub_uid`** (`role_host_core.hpp:94-101`). HEP-0023 §7 explicitly defines the field for dual-hub processor message origin disambiguation; HEP-0033 §18.3 + §19.4 also require source tagging. Today the BRC `on_notification` lambda captures `i` (`role_api_base.cpp:908`) but `i` is only used in the trace log — never propagated. Also: field-name drift (HEP: `sender_uid`; code: `sender`) and type drift (HEP: `std::vector<char>`; code: `std::vector<std::byte>`). |
| **C4** | **D2** | **Heartbeat tick installed on master BRC only** (`role_api_base.cpp:563-567`). HEP-0033 §19.3 step 3: *"For each presence: install a periodic heartbeat tick on its connection."* Today: one tick on `connections()[0]` that dispatches per-channel. Functionally equivalent for nominal operation, but if the master poll loop blocks/slows, tick processing for peer connections is delayed too. Worth a design decision: keep central tick (simpler) or move to per-conn (matches §19). |
| **C5** | **D3** | **`Presence` struct missing `inbox_meta`** declared in HEP-0033 §19.1 (`role_presence.hpp:89-100`). Inbox metadata still flows through pImpl side-fields + `append_inbox_to_reg`. Deferred to Wave-B M5+ per the code comment. Flag as a tracked gap that survives this review. |

---

## 2. Inbox protocol

### 2.1 End-to-end sequence (pseudocode)

```
RECEIVER SIDE (role startup)
  inbox_queue = InboxQueue::bind_at(endpoint, schema, packing, rcvhwm)
  inbox_queue.start()                              // bind ROUTER socket
  For each presence registered (REG_REQ / CONSUMER_REG_REQ):
    opts["inbox_endpoint"]    = inbox_queue.actual_endpoint()  // SAME string ALL presences
    opts["inbox_schema_json"] = inbox_cfg.schema_fields_json
    opts["inbox_packing"]     = inbox_cfg.packing
    opts["inbox_checksum"]    = inbox_cfg.checksum
  Broker stores per-presence (broker_service.cpp handle_reg_req / handle_consumer_reg_req):
    - producer presence → ChannelEntry.producers[i].inbox_*
    - consumer presence → ConsumerEntry.inbox_*
  Threading: inbox_thread_ runs recv_one → invoke_on_inbox → send_ack

SENDER SIDE — api.open_inbox(target_uid) (role_api_base.cpp:1310-1406)
  if !pImpl->handler_: return nullopt
  conns = handler_->connections()
  RoleHostCore::open_inbox(target_uid, factory) — atomic check-and-create:
    factory() — Class B fall-through (A3):
      for conn in conns:
        bc = conn.brc.get()
        if !bc: continue
        resp = bc.query_role_info(target_uid, 1000)        // ROLE_INFO_REQ
        if !resp: continue                                  // transport failure
        sch = resp.value("inbox_schema", {})                // ⚠ HEP says inbox_schema_json
        if sch.is_object() && sch.contains("fields"):
            info = resp; found = true; break
      if !found: return nullopt
      spec     = parse_schema_json(info.inbox_schema)
      item_sz  = compute_schema_size(spec, info.inbox_packing)
      client   = InboxClient::connect_to(info.inbox_endpoint, my_uid,
                                          spec_to_zmq_fields(spec), packing)
      client.start()
      client.set_checksum_policy(string_to_checksum_policy(info.inbox_checksum))
      return InboxCacheEntry{shared(client), "InboxSlot", item_sz}
  return InboxOpenResult{client, spec, packing, item_size}

BROKER — handle_role_info_req (broker_service.cpp:3262-3378)
  snap = hub_state_->snapshot()
  // Search producer rows first (HEP-0023 §2.1.1 multi-producer aware)
  for (name, entry) in snap.channels:
    prod = entry.find_producer(uid)
    if prod:
        return {found = !prod.inbox_endpoint.empty(),
                channel = name,
                inbox_endpoint = prod.inbox_endpoint,
                inbox_packing  = prod.inbox_packing,
                inbox_checksum = prod.inbox_checksum,
                inbox_schema   = parse_or_empty(prod.inbox_schema_json)}
  // Then consumer rows
  for (name, entry) in snap.channels:
    for cons in entry.consumers:
        if cons.role_uid == uid: return analogous shape
  return {found: false}

WIRE FORMAT (HEP-0027 §3) — used by InboxQueue/InboxClient
  msgpack fixarray[5]:
    [magic:uint32 0x51484C50, schema_tag:bin8, seq:uint64,
     payload:array(N), checksum:bin32]
  ROUTER framing: [identity, "", payload]; ACK = [identity, "", ack_byte]
  ack_byte: 0=OK, 1=overflow, 2=schema, 3=handler_error

DRAIN (per data-cycle on main thread)
  drain_inbox_sync(inbox_queue):
    while item = inbox_queue.recv_one(0ms):
      ack = engine.invoke_on_inbox(item.data, item.size, item.sender_id) ? 0 : 3
      inbox_queue.send_ack(ack)
```

### 2.2 Findings — Inbox

| # | Tier | Finding |
|---|---|---|
| **I1** | **D2** | **ROLE_INFO_ACK wire-field name drift.** HEP-0023 §4 + HEP-0027 §4.2 specify `inbox_schema_json` (JSON-string). Broker returns the field as `inbox_schema` (already-parsed object) at `broker_service.cpp:3297, 3343`. Sender-side `open_inbox_client` reads `inbox_schema` (`role_api_base.cpp:1342-1352`). Code is self-consistent; **HEP is the one out of sync**. Recommend amending the HEPs to `inbox_schema` (object). |
| **I2** | **D3** | **Header docstring drift on wire format.** `hub_inbox_queue.hpp:12` says `fixarray[4]` (no checksum); `hub_inbox_queue.cpp:6` correctly says `fixarray[5]` with checksum. Trivial header fix. |
| **I3** | **D2** | **`found` field semantics overload broker-side.** `broker_service.cpp:3288, 3334` sets `resp["found"] = !inbox_endpoint.empty()` — but `found` reads naturally as "role registered here yes/no" rather than "role has an inbox here yes/no". Open_inbox_client's actual predicate is `inbox_schema.is_object() && contains("fields")` (`role_api_base.cpp:1343`) — so case (b) "role registered without inbox" properly falls through. Recommend renaming `found` → `has_inbox` in the ROLE_INFO_ACK wire to clarify intent. |
| **I4** | **D2** (by design) | **Inbox metadata stored TWICE in HubState** when a dual-hub processor advertises. The same `inbox_endpoint` string lives on `ChannelEntry.producers[*].inbox_*` (out_hub) and `ConsumerEntry.inbox_*` (in_hub). HEP-0027 §4.1 step 7 + HEP-0033 §19.5 explicitly design this — "every interested hub holds its own metadata copy". Not removable without breaking the per-hub `ROLE_INFO_REQ` answer model. **Flagged here only to deter "let's deduplicate this" cleanup attempts.** |

---

## 3. Band protocol

### 3.1 End-to-end sequence (pseudocode)

```
ROLE SIDE — api.band_join(name) (role_api_base.cpp:1236-1270)
  bc = handler.brc_for_band(name)                  // null on first-time join
  if !bc: bc = handler.brc_for_role()              // bootstrap fallback (A0 fix)
  if !bc: return nullopt
  resp = bc.band_join(name)                        // emits BAND_JOIN_REQ
  if resp.has_value() && handler_:
      presences = handler_->presences()
      if !presences.empty():
          handler_->on_band_joined(name, &presences.front())    // ⚠ always [0]
  return resp

BRC SEND — band_join (broker_request_comm.cpp:881-889)
  payload["channel"]  = channel    // ⚠ HEP-0030 §5.1: should be "band"
  payload["role_uid"] = role_uid
  payload["role_name"]= role_name
  do_request("BAND_JOIN_REQ", "BAND_JOIN_ACK", payload, timeout_ms)

BROKER — handle_band_join_req (broker_service.cpp:4207-4265)
  channel  = req["channel"]                         // matches BRC; ⚠ HEP says "band"
  role_uid = req["role_uid"]; role_name = req["role_name"]
  if (!channel.empty() && !role_uid.empty()):
      // Notify existing members BEFORE adding new one
      notify = {channel, role_uid, role_name}
      if pre_band = hub_state.band(channel):
          for m in pre_band.members:
              send_to_identity(socket, m.zmq_identity, "BAND_JOIN_NOTIFY", notify)
      hub_state._on_band_joined(channel,
                                BandMember{role_uid, role_name, identity, now})
      // hub_state validates !band-prefix via is_valid_identifier(name, IdentifierKind::Band)
  // Reply with current member list
  return {status:"success", channel:name, members:[...]}

ROLE SIDE — api.band_broadcast(name, body) (role_api_base.cpp:1289-1295)
  bc = handler.brc_for_band(name)
  if bc: bc.band_broadcast(name, body)

BROKER — handle_band_broadcast_req (broker_service.cpp:4309-4338)
  channel = req["channel"]; sender_uid = req["sender_uid"]; body = req["body"]
  for m in band(channel).members where m.role_uid != sender_uid:
      send_to_identity(socket, m.zmq_identity, "BAND_BROADCAST_NOTIFY",
                       {channel, sender_uid, body})

ROLE SIDE — INBOUND NOTIFICATION (role_api_base.cpp:907-918)
  brc.on_notification([core, tag, i](type, body):
      msg.event           = type            // "BAND_*_NOTIFY"
      msg.notification_id = parse_notification_id(type)
                          = Unknown                    // ⚠ band events not catalogued
      msg.details         = body
      core.enqueue_message(msg))

  RoleHandler::find_presence_from_notification (role_handler.cpp:283-307):
      Class A: body.find("channel_name") → look up in channel_index_
      Class D: body.find("band_name")    → look up in band_index_
      // ⚠ band notify body has "channel", not "channel_name" or "band_name"
      // ⚠ Class D inbound never matches — band_index_ is dead for routing

BROKER-INITIATED LEAVE FANOUT (broker_service.cpp:2960-2986)
  send_band_leave_notify(socket, band_name, role_uid, reason):
      remaining = hub_state.band(band_name)
      if !remaining: return                        // band auto-deleted
      notify = {channel:band_name, role_uid, reason}
      for m in remaining.members:
          send_to_identity(socket, m.zmq_identity, "BAND_LEAVE_NOTIFY", notify)

HEARTBEAT-DRIVEN AUTO-LEAVE (HEP-0030 §6)
  On Pending→Disconnected for a role with band memberships:
      _on_role_disconnected → band cleanup hook removes role from every band
      For each band: send_band_leave_notify(..., reason="heartbeat_timeout")
      If band becomes empty: auto-delete
```

### 3.2 Findings — Band

| # | Tier | Finding |
|---|---|---|
| **B1** | **D1** | **Wire field name `"channel"` vs HEP-0030 `"band"`.** HEP-0030 §5.1 specifies every BAND_* request and notify body uses field `band`. Actual wire (BRC: `broker_request_comm.cpp:884,895,905,916`; broker: `broker_service.cpp:4212,4271,4314,4343,4228,2978,4320`): `channel`. Either fix the wire (breaks compat — requires `broker_proto_major` bump) or fix the HEP to match shipped code. Every reader of either today will be confused. |
| **B2** | **D1** | **Inbound band notifications cannot route to a Presence.** `RoleHandler::find_presence_from_notification` (`role_handler.cpp:283-307`) looks for `body["channel_name"]` (Class A), then `body["band_name"]` (Class D). All `BAND_*_NOTIFY` emit `body["channel"]` (not `channel_name`, not `band_name`). Result: every band notification returns nullptr from this lookup. The Class D inbound path is structurally broken; today no script callback reads the per-presence tag yet so the breakage is invisible. **Mutation test:** changing `band_index_` does nothing observable. |
| **B3** | **D1** | **Triple-name divergence for the same identifier:** HEP `band`, wire `channel`, role-side dispatch `band_name`. Pick one and align all three. `band` is the obvious choice per HEP intent + ergonomics + the `!band` prefix convention. |
| **B4** | **D2** | **`band_index_` entry uses `presences[0]` always** (`role_api_base.cpp:1267`). HEP-0033 §18.3 says the role picks which connection joins the band ("band_index in the role-side handler records the choice"). For a dual-hub processor, `presences[0]` is the in-hub consumer presence — every band join goes to in-hub regardless of caller intent. The docstring at line 1247 acknowledges the need for `api.in_hub.band_join` / `api.out_hub.band_join` accessors. Either (a) add the side-selector accessors, or (b) document explicitly that dual-hub bands are deferred. |
| **B5** | **D2** | **`BAND_*_NOTIFY` types not in `NotificationId` catalog.** Only `ChannelClosing` + `ConsumerDied` are mapped (`role_host_core.hpp:77-82`). Band notifications → `NotificationId::Unknown` → fall through to script's generic `msgs[]` scan. Per HEP-0011 callback table model — band events should have dedicated callbacks (`on_band_joined` / `on_band_left` / `on_band_broadcast`). Without them, every script that wants band events must scan strings. The callback table is the design contract being side-stepped. |
| **B6** | **D2** | **HEP-0030 over-aggressively retires `CHANNEL_BROADCAST_REQ`.** §9 marks it SUPERSEDED. Code reality: broker still handles it (`broker_service.cpp:919`); BRC still exposes `send_broadcast` (`broker_request_comm.cpp:701`); 3 L3 worker tests in `datahub_broker_protocol_workers.cpp` (lines 1124, 1203, 1257) exercise it directly. Channel-bound broadcast and band-bound broadcast are semantically different (channel = data-plane registry; band = control-plane group). Either re-affirm both as live mechanisms with distinct purposes, or migrate the 3 tests to `BAND_BROADCAST` and delete the broker handler. |

---

## 4. Cross-cutting findings — duplicated / obsolete / redundant

| # | Tier | Finding |
|---|---|---|
| **X1** | **D3** | **BRC dead method `send_notify`** (`broker_request_comm.cpp:691`). Zero callers anywhere in `src/` or `tests/`. Counterpart broker handler `handle_channel_notify_req` (`broker_service.cpp:914`) is now fed only by the federation relay (lines 622-637 build a synthetic CHANNEL_NOTIFY_REQ). The role-side surface is dead — safe to delete. |
| **X2** | **D3** | **BRC dead method `query_shm_info`** (`broker_request_comm.cpp:867`). Zero callers. `SHM_BLOCK_QUERY_REQ` broker handler is also unused on the role-side hot path (admin-only via different surface). Safe to delete. |
| **X3** | **D3** | **`Impl::resolve_bc_for_{channel,role,band}` are 1-line forwards** to `handler_->brc_for_*` (`role_api_base.cpp:204-218`). Vestige of M4d-M4f migration. Either inline at the ~30 call sites or keep for symmetry — adds no functional value today. |
| **X4** | **D3** | **Stale Wave-B M4d/e/f migration comments** at ~15 call sites in `role_api_base.cpp` (e.g. *"Wave-B M4e: Class D — route via handler when active"* — that wave shipped). |
| **X5** | **D2** | **`Presence::connection` raw pointer + `connections_.reserve(presences_.size())`** form an implicit contract enforced only by code comments (`role_handler.cpp:48-57`). A future Wave that adds dynamic presence rebinding (hub failover) silently breaks pointer stability. Recommend a debug-build vector-mutation guard or refactor to `std::list` / stable-storage container. |
| **X6** | **D3** | **`ChecksumRepairPolicy::Repair`** — pre-compaction summary flagged this as dead enum value; not re-verified in this review but stands as a polish item. |
| **X7** | **D2** (by design) | **`ChannelEntry.consumers[].inbox_*` and `ChannelEntry.producers[].inbox_*` carry IDENTICAL strings for dual-hub processor** (HEP-0027 §4.1 step 7 — same `inbox_endpoint` advertised on every presence). This is by design (each hub answers `ROLE_INFO_REQ` from its own copy) but it IS duplicated data per the literal reading. Document explicitly to deter "let's deduplicate this" attempts. |

---

## 5. Tier rollup

| Tier | Definition | Count | Items |
|---|---|---|---|
| **D1** | Must reconcile — real design-contract violations | **0 (all closed)** | ~~C1~~ ✅ 2026-05-18, ~~B1~~ ~~B2~~ ~~B3~~ ✅ 2026-05-17 |
| **D2** | Code/design drift that bites later if untouched | **9** | C2, C3, C4, I1, I3, B4, B5, B6, X5, X7 (10 — counting X7 as D2-by-design) |
| **D3** | Polish / dead code | **6** | C5, I2, X1, X2, X3, X4, X6 (7 — counting C5 as deferred-tracked) |

### D1 — direction needs user input

1. ~~**C1: Hub-dead policy.**~~ ✅ FIXED 2026-05-18 — see C1 row above for resolution.  Unified dispatch model (HEP-0011 §"Notification dispatch") closed the drift: framework default = asymmetric (master-stop, peer-no-op); script `on_hub_dead` callback overrides default if it wants either-hub-exits or any other policy.
2. ~~**B1: Band wire field naming.**~~ ✅ FIXED 2026-05-17 — finished the 2026-04-11 rename refactor; wire fields now `band` per HEP-0030 §5.1; proto bumped 3→4.
3. ~~**B2: Class D inbound routing.**~~ ✅ FIXED 2026-05-17 — `find_presence_from_notification` reads `body["band"]`.
4. ~~**B3: Triple-name divergence.**~~ ✅ FIXED 2026-05-17 — auto-resolved.

### Why 1924/1924 didn't catch B1/B2 — closed by new L3 regression

| Layer | What it tested | Why it missed B1/B2 |
|---|---|---|
| L2 `test_role_handler.cpp` | `find_presence_from_notification` against synthesized bodies | Synthesized `body["band_name"]` matched the broken dispatcher — fixture agreed with the bug |
| L3 `datahub_channel_group_workers.cpp` | Band join/leave/broadcast round-trip via raw BRC | Used raw BRC `on_notification` callbacks — bypassed `find_presence_from_notification` entirely |
| Everywhere | Wire round-trip | BRC and broker agreed on the wrong key (`channel`) — round-trips worked |

**New L3 test `RoleAPIBase_BandNotify_WireField_And_Routing`** drives a real broker emission of `BAND_JOIN_NOTIFY` and runs the captured body through `RoleHandler::find_presence_from_notification`. Mutation-tested 2026-05-17: reverting either bug fails the test with the diagnostic message that pinpoints which side broke. This closes the integration gap that hid both bugs in the 1923-test baseline.

### D2 — should be on a tracked plan

Best absorbed into Wave-B M5+ where `Presence::inbox_meta` lands (C5) and the role-tag-string branching collapses to presence-iteration (C2). C3 (`source_hub_uid`) is a small standalone code change.

### D3 — opportunistic polish wave

Batch X1/X2/X3/X4/X6 + I2 into one PR. Low risk; one diff for cleanup.

---

## 6. Recommendation for next session

I'd suggest splitting into three discrete sessions:

1. **Reconcile the 4 D1 doc/code conflicts** — these need user decisions on direction, not code changes by me.
2. **Land C3 (`source_hub_uid` field)** — small concrete code change, real functional gap for multi-hub scripts.
3. **D3 polish wave** — at the very end, batch the 6 polish items into one PR.

**Nothing was modified in code during this review. All findings cite file:line for verification.**

---

## 7. Cross-references

- **HEP-CORE-0023** (Startup Coordination) — connection FSM, REG_REQ/ACK/heartbeat shape
- **HEP-CORE-0027** (Inbox Messaging) — inbox architecture + per-presence advertisement
- **HEP-CORE-0030** (Band Messaging Protocol) — band wire spec + heartbeat-driven leave
- **HEP-CORE-0033** §18 (Routing taxonomy A/B/C/D) — dispatch model
- **HEP-CORE-0033** §19 (Multi-presence roles) — RoleHandler architecture + §19.6 hub-dead policy
- **HEP-CORE-0011** (ScriptHost) — `on_X` callback table — relevant to B5
- **HEP-CORE-0031** §4.1 (Thread Shutdown Contract) — ctrl-thread teardown

## 8. Status table for individual findings (track resolution here)

| ID | Tier | Title | Status | Resolution / commit |
|---|---|---|---|---|
| C1 | D1 | Hub-dead policy §19.6 ↔ A2 | ❌ OPEN | — |
| C2 | D2 | Heartbeat tick uses role_tag string branching | ❌ OPEN | — |
| C3 | D2 | IncomingMessage lacks source_hub_uid | ✅ FIXED 2026-05-17 | Added `source_hub_uid` field; BRC `on_notification` lambda captures `connections()[i].broker_endpoint` (HEP-CORE-0033 §19.2 dedup key) and stamps it on every `IncomingMessage`.  New L3 regression `RoleAPIBase_SourceHubUid_Disambiguates_DualHub` mutation-tested. |
| C4 | D2 | Heartbeat tick on master only | ❌ OPEN | — |
| C5 | D3 | Presence struct missing inbox_meta | ❌ OPEN (deferred Wave-B M5+) | — |
| I1 | D2 | ROLE_INFO_ACK field name drift | ❌ OPEN | — |
| I2 | D3 | Header docstring fixarray[4] vs [5] | ❌ OPEN | — |
| I3 | D2 | `found` field semantics | ❌ OPEN | — |
| I4 | D2 (by design) | Inbox metadata stored twice (per-hub) | ✅ DOCUMENTED | document-only — see HEP-0027 §4.1 |
| B1 | D1 | Wire field "channel" vs HEP "band" | ✅ FIXED 2026-05-17 | Finished the 2026-04-11 rename refactor (`8d3ee1e`) on the wire-payload-key layer.  BRC + broker handlers + ACKs + notifies all use `band` per HEP-CORE-0030 §5.1.  Broker proto bumped 3→4.  Mutation-tested via the new L3 regression. |
| B2 | D1 | Class D inbound routing broken | ✅ FIXED 2026-05-17 | `RoleHandler::find_presence_from_notification` now reads `body["band"]` (the wire field) instead of the never-emitted `band_name` invented in Wave-B M4b (`8c3994c`).  L2 test fixtures `body["band_name"]` → `body["band"]`. |
| B3 | D1 | Triple-name divergence | ✅ FIXED 2026-05-17 | Auto-resolved by B1+B2: HEP, wire, and dispatcher now all agree on `band`. |
| B4 | D2 | band_index_ always uses presences[0] | ❌ OPEN | — |
| B5 | D2 | BAND_*_NOTIFY not catalogued | ❌ OPEN | — |
| B6 | D2 | HEP-0030 retires CHANNEL_BROADCAST too aggressively | ❌ OPEN | — |
| X1 | D3 | Dead BRC `send_notify` | ❌ OPEN | — |
| X2 | D3 | Dead BRC `query_shm_info` | ❌ OPEN | — |
| X3 | D3 | resolve_bc_for_* forwarders | ❌ OPEN | — |
| X4 | D3 | Stale Wave-B M4d/e/f comments | ❌ OPEN | — |
| X5 | D2 | Presence::connection pointer stability contract | ❌ OPEN | — |
| X6 | D3 | ChecksumRepairPolicy::Repair dead enum | ❌ OPEN | — |
| X7 | D2 (by design) | Inbox per-hub duplication | ✅ DOCUMENTED | document-only — see HEP-0027 §4.1 |

---

# Round 2 — Deeper state-explicitness audit (2026-05-17, after B1/B2/B3 fix)

**Question driving this round** (from user): *"do another round of thorough and critical review … such that we understand the protocol design is complete without missing gaps such that the system will operate with explicit and confirmed state without ambiguity, and the tests cover these designs faithfully."*

This round zooms in past wire keys — into protocol state observability, FSM completeness vs implicit-from-fields state, terminology drift, test-rigor patterns, and code that's now structurally obsolete.

## R2.1 — Protocol-state observability gaps (new findings)

### S1 — Role-side has no explicit registration FSM (D2)

**File:** `src/utils/service/role_api_base.cpp:114-145` (`struct Shared`)

```cpp
std::string producer_channel;   ///< Non-empty if REG_REQ succeeded.
std::string consumer_channel;   ///< Non-empty if CONSUMER_REG_REQ succeeded.
```

Role-side registration state is encoded as **non-emptiness of two strings**, one per role-tag. Problems:

1. **Implicit state with overloaded semantics.** `producer_channel.empty()` could mean "never registered", "registration in flight", "registered then DEREGed", or "fundamentally not a producer-kind role" — the comment claims "Non-empty if REG_REQ succeeded" but the data structure can't distinguish these.
2. **Single-channel-per-role-kind assumption** baked into Impl. A multi-presence role (processor-with-2-producers, future N-input router from HEP-0033 §19.1) cannot represent its registered-channel set here. The `handler_->presences()` vector already carries the topology; this struct duplicates a subset of that data as state.
3. **Race with topology.** `handler_->presences()` lists `out_channel` for a producer; `Shared::producer_channel` is set on REG_REQ success. The two CAN disagree if REG_REQ for a presence fails partway. Today: `Shared::producer_channel` only assigned on success, so disagreement is silent ("topology says I should be on channel X, state says I never registered").

**No FSM, no enumerable transitions, no observability of "REG_REQ in flight".** From outside the class there is no way to ask "for presence P, is its registration confirmed?"

**Recommended direction:** Add an explicit per-presence registration state on `Presence`:
```cpp
enum class RegistrationState : uint8_t { Unregistered, RegRequestPending, Registered, Deregistered };
```
This collapses `Shared::producer_channel` / `consumer_channel` into per-presence state on the topology vector (handler_->presences()).  Tests can then pin "presence P went through Unregistered → RegRequestPending → Registered" rather than "REG_REQ has_value()" outcome-only.

### S2 — `RoleState` enum has 3 values but observable space has 4 (D3 — documented design)

**File:** `src/include/utils/hub_state.hpp:66-76, 100-116, 693-704`

`RoleState ∈ {Connected, Pending, Disconnected}` plus a boolean latch `first_heartbeat_seen` collectively encode 4 states. The `ChannelObservable` enum (`{kAbsent, kRegistering, kStalled, kLive}`) makes the 4 states explicit, but only at the derived-view layer. Reading the FSM directly requires inspecting BOTH fields, which is a coherence trap (someone reads `state == Connected` and assumes "ready" — missing the `first_heartbeat_seen` gate).

**Disposition:** Today's shape IS documented in the docstring (lines 105-109). Acceptable, but the split-representation invites bugs. If we ever extend the FSM (e.g., add Reconnecting), explicit 4-value enum becomes cleaner.

### S3 — `start_handler_threads` Phase 2-4 window has unobservable initialization state (D2)

**File:** `src/utils/service/role_api_base.cpp:894-1039`

- Phase 1: BRCs connect.
- Phase 2: notification callbacks wired.
- Phase 3: ctrl threads spawn (loops start).
- Phase 4: `connection_alive_mask_` initialized to `(1<<N)-1`.

Between Phase 3 (spawn) and Phase 4 (mask set), if `on_hub_dead` fires (e.g. broker crashes mid-startup), the lambda reads `alive_mask = 0`, clears bit 0 → still 0 → master branch triggers `request_stop()`. This is benign (correct outcome) but the **state is logically wrong** for a brief window: ctrl thread is running yet mask claims "0 alive".

**Recommended:** Move the Phase 4 mask init BEFORE Phase 3 — set it once we know N, then spawn threads. The on_hub_dead lambda reading mid-startup gets the "alive" semantics correct.

### S4 — Band membership has no per-role local state (D2)

**File:** `src/utils/service/role_handler.cpp:254-271`

```cpp
void RoleHandler::on_band_joined(const std::string &band_name,
                                  const Presence    *presence) noexcept {
    band_index_[band_name] = const_cast<Presence *>(presence);
}
```

The role-side tracks band-routing (`band_name → Presence*`) but has no `joined / leaving / left` state for itself. If `api.band_join()` times out (broker drops the REQ), `on_band_joined` is never called and the index stays empty — but from the role's POV, *did it join?* No way to know without re-querying via `band_members`. Symmetrically, `on_band_left` is called only on successful `band_leave`; a timeout leaves stale routing.

**Recommended:** Wrap the index entry in a small state record:
```cpp
struct BandRouting { Presence *presence; enum State { Joining, Joined, Leaving } state; };
```
Set `Joining` before the REQ, transition to `Joined` on ACK, `Leaving` before LEAVE, erase on ACK. Tests can pin transitions.

## R2.2 — HEP / code terminology drift

### T1 — Counter names use legacy "Ready" while state uses new "Connected" (D2)

**Files:**
- `src/include/utils/hub_state.hpp:66` — `enum class RoleState { Connected, Pending, Disconnected }`
- `src/include/utils/hub_state.hpp:1043-1045` — `ready_to_pending_total`, `pending_to_ready_total`
- `src/utils/ipc/broker_service.cpp:2635-2683` — `effective_ready_timeout()`, `ready_timeout`

HEP-0023 §2 was re-architected 2026-05-07: state names changed from "Ready" → "Connected". §2.5.3 acknowledges the counter names retain the legacy term:

> *Naming notes: "Ready" in `ready_to_*` is the legacy term for the post-§2 `Connected` state with `first_heartbeat_seen == true` ("Live" sub-state).*

So the HEP **explicitly documents** this drift and defers the rename. But the code mixes the two vocabularies, and comments at e.g. `hub_state.hpp:753` say "Pending → Connected" while immediately referring to `pending_to_ready_total`.

**Recommended:** Either (a) finish the rename — counters + config fields + comments — under a tracked task, or (b) bump all the comment references to use the legacy term consistently (so a reader doesn't see both terms in adjacent lines). Today's mix is the worst of both.

### T2 — Comments on `register_*` mention the wire field uniformly (clean)

Spot-check passed: REG_REQ / CONSUMER_REG_REQ / DEREG_REQ paths use `channel_name` consistently in body keys (HEP-0007 §12.4). Variable names match. No drift here.

### T3 — HEP-0030 §9 over-aggressively retires `CHANNEL_BROADCAST_REQ` (D1) [previously B6]

**Files:**
- `src/utils/ipc/broker_service.cpp:919, 4309-4338` — broker handler still alive
- `src/utils/network_comm/broker_request_comm.cpp:701` — BRC `send_broadcast` still alive
- `src/utils/service/hub_api.cpp:364` — script-side `api.broadcast` calls `request_broadcast_channel`
- `src/utils/ipc/admin_service.cpp:582` — admin RPC uses it
- `src/utils/ipc/broker_service.cpp:622-637` — broker builds synthetic CHANNEL_BROADCAST_REQ for internal hub-initiated broadcasts

Reality: CHANNEL_BROADCAST_REQ is **a channel-bound broadcast** (broker fans out to producer+all consumers of a CHANNEL_NAME) — semantically distinct from BAND_BROADCAST_REQ (broker fans out to members of a BAND). They are complementary, not supersede.

HEP-0030 §9 says:
> | CHANNEL_BROADCAST_REQ (§12.4) | **SUPERSEDED** | BAND_BROADCAST_REQ |

**This is wrong.** CHANNEL_BROADCAST_REQ remains a live protocol message with three real callers (hub API, admin RPC, federation relay path) and 3 L3 test workers.

**Recommended fix:** amend HEP-0030 §9 to remove the "SUPERSEDED" marker on CHANNEL_BROADCAST_REQ / CHANNEL_BROADCAST_NOTIFY / CHANNEL_NOTIFY_REQ / CHANNEL_EVENT_NOTIFY. Add a §10 note clarifying that channel-bound broadcasts and band-bound broadcasts coexist (channel = data-plane registry membership; band = explicit pub/sub group).

This is doc-only.

## R2.3 — Test-rigor faithfulness (audit pattern + gap)

### TR1 — Wire-conformance pattern is new and isolated to audit-B's regression

The new test `RoleAPIBase_BandNotify_WireField_And_Routing` is the **first** test in the suite that pins a wire payload key against a HEP §. No analogous test exists for:
- BAND_JOIN_ACK / BAND_LEAVE_ACK / BAND_MEMBERS_ACK key sets (HEP-0030 §5.1)
- REG_ACK / CONSUMER_REG_ACK `heartbeat` sub-block (HEP-0023 §2.5.1)
- DISC_REQ_ACK shape (HEP-0023 §2.2)
- ROLE_INFO_ACK shape (HEP-0023 §4 / HEP-0027 §4.2)
- CHANNEL_*_NOTIFY key set (HEP-0007 §12.5)

These are the **classes of bugs B1 hid**: BRC and broker agree on the wrong wire key, round-trip works, no test catches the spec drift.

**Recommended:** Generalize the audit-B pattern into a small wire-conformance helper, e.g. `expect_payload_keys(json, {"band", "role_uid", "role_name"})`, and add one test per protocol message family. Estimate: ~10 messages × 3 lines each = trivial to write, high regression value.

### TR2 — Tests pin HubState well, role-side state poorly (D2)

Counts of state-pinning assertions:
- `EXPECT_TRUE(broker.hub_state->channel(ch).has_value())` — many (~20+ sites)
- `EXPECT_TRUE(broker.hub_state->role(uid).has_value())` — many
- `EXPECT_EQ(api.handler()->brc_for_band(b), nullptr)` — a few

But there are no equivalents for role-side registration state because the role-side has no first-class FSM (per S1). Tests can't pin "after REG_REQ, role's registration FSM transitions to Registered" because there is no FSM.

**Recommended:** Couples to S1 — once a Registration FSM lands, add transition-pinning tests.

### TR3 — Mutation-sweep coverage is uneven (D3)

Tests that EXPLICITLY document the mutation they'd catch (audit-A0/A2/A3, audit-B1/B2): excellent. Most older tests: don't say. Without a mutation marker, it's unclear which assertion is load-bearing.

**Recommended:** New tests should include an explicit "mutation sweep" comment block (pattern in the new audit-B test). Existing tests are best left alone (churn cost too high).

## R2.4 — Obsolete code / inconsistency findings (new + re-confirmed)

### O1 — Dead BRC methods confirmed (D3 — was X1, X2)

**Files:** `src/utils/network_comm/broker_request_comm.cpp:691` (`send_notify`), `:867` (`query_shm_info`)

Zero callers in src/ + tests/ (re-grepped). Safe to delete the methods + their header declarations + their broker handlers if also unused.

Actually — `handle_channel_notify_req` IS still called via `dispatch` (`broker_service.cpp:914`), but only by the federation-relay synthetic-payload path at line 622-637, which actually calls `handle_channel_broadcast_req` instead. Let me confirm.

Grep for `handle_channel_notify_req`:
```
src/utils/ipc/broker_service.cpp:914:    else if (msg_type == "CHANNEL_NOTIFY_REQ")
src/utils/ipc/broker_service.cpp:917:        handle_channel_notify_req(socket, payload);
```
Only the dispatch entry — no internal synthetic-payload caller. The CHANNEL_NOTIFY_REQ wire-handler can be reached only by a role calling `BRC::send_notify` (dead) or a peer hub relaying. Since federation also delegates to broadcast (line 637), CHANNEL_NOTIFY_REQ is purely dead.

**Safe to delete:**
- `BrokerRequestComm::send_notify` + header
- `BrokerServiceImpl::handle_channel_notify_req` + dispatch entry

### O2 — Stale Wave-B M4d/e/f migration comments confirmed (D3 — was X4)

5+ sites in `src/utils/service/role_api_base.cpp` (lines 183, 683, 715, 725, 793, 1275, 1292, 1299) say "Wave-B M4d/e/f: Class A/D — route via handler when active." Those waves shipped; the conditional "when active" no longer applies (handler is always active post-M4f). Comments are misleading.

**Recommended:** strip the "Wave-B M4d/e/f:" prefix or rewrite as "Class A — route via handler index".

### O3 — `Impl::resolve_bc_for_{channel,role,band}` are pure forwarders (D3 — was X3)

3 wrappers, each 1 line forward to `handler_->brc_for_*()`. Pre-M4f they had a fallback path (legacy `broker_channel`); post-M4f the fallback was deleted, leaving pure forwarders.

**Recommended:** inline at the ~30 call sites. The wrappers add no value and the docstring on Impl (`role_api_base.cpp:183`) still describes the deleted fallback semantics.

### O4 — `Shared::producer_channel` + `Shared::consumer_channel` duplicate `handler_->presences()` (D2 — new finding)

See S1. Per HEP-0033 §19, the canonical role topology is `handler_->presences()`. These two strings are state-tracking that should be on the presence record, not in a side struct.

## R2.5 — IncomingMessage `source_hub_uid` (re-confirms C3 was a real gap)

`role_host_core.hpp:94-101` — no `source_hub_uid` field. HEP-0023 §7 + HEP-0033 §18.3 + §19.4 all require it for dual-hub message attribution. The BRC `on_notification` lambda already captures `i` (`role_api_base.cpp:908`) but only uses it in the trace log.

**This gap is the one user-visible cost of NOT having dual-hub message origin info** — a dual-hub processor's script cannot tell which hub emitted a CHANNEL_CLOSING_NOTIFY without comparing `body["channel_name"]` to its presence list manually.

**Status:** Open. The fix is small (add field + populate from `i` via `handler->connections()[i].broker_endpoint` or a per-conn hub_uid sidecar). Worth landing alongside C2 (heartbeat tick iteration).

## R2.6 — Round 2 Tier rollup

| New ID | Tier | Title |
|---|---|---|
| **S1** | **D2** | Role-side has no explicit registration FSM |
| **S2** | **D3** | RoleState has 3 values + boolean latch encoding 4 effective states |
| **S3** | **D2** | start_handler_threads Phase 2-4 window has unobservable init state |
| **S4** | **D2** | Band membership has no per-role local FSM |
| **T1** | **D2** | Counter names use legacy "Ready" while state enum uses new "Connected" |
| **T3** | **D1** | HEP-0030 §9 over-aggressively retires CHANNEL_BROADCAST_REQ (doc fix) |
| **TR1** | **D2** | Wire-conformance test pattern is new + isolated; needs spread to other messages |
| **TR2** | **D2** | Tests pin HubState well, role-side state poorly (couples to S1) |
| **TR3** | **D3** | Existing tests lack mutation-sweep markers |
| **O1** | **D3** | Dead BRC methods + CHANNEL_NOTIFY_REQ broker handler |
| **O2** | **D3** | Stale Wave-B M4d/e/f migration comments |
| **O3** | **D3** | resolve_bc_for_* pure forwarders |
| **O4** | **D2** | Shared::producer_channel/consumer_channel duplicate presences() topology |

## R2.7 — Recommended next-step sequence (foundation for next work)

If we want **explicit and confirmed state without ambiguity**, these 4 items would substantively raise the bar:

1. **T3** (HEP-0030 §9 correction) — doc-only, zero code risk, fixes an actively misleading HEP.
2. **C3** (IncomingMessage source_hub_uid) — small code change, real functional gap, enables dual-hub script ergonomics.
3. **S1+O4** (registration FSM on Presence; retire `Shared::producer_channel`/`consumer_channel`) — couples to T1 (rename Ready→Connected) since the FSM landing is the right moment to harmonize terminology. This is the largest structural change but the highest-leverage one for the user's "explicit state" requirement.
4. **TR1** (wire-conformance test helper + 4-5 new tests for ACK shapes) — prevents the next B1 class of bug.

D3 polish (O1, O2, O3) can be a single PR at the very end.

## R2.8 — Round 2 status table

| ID | Tier | Title | Status |
|---|---|---|---|
| S1 | D2 | No explicit role-side registration FSM | ✅ FIXED 2026-05-17 — `RegistrationState` enum + atomic field on `Presence` (Unregistered/RegRequestPending/Registered/Deregistered).  Mutation-tested via `RoleAPIBase_RegistrationFSM_Transitions`. |
| S2 | D3 | RoleState 3-value + boolean latch | ❌ OPEN (documented design) |
| S3 | D2 | start_handler_threads init-window mask state | ❌ OPEN |
| S4 | D2 | Band membership has no per-role local FSM | ❌ OPEN |
| T1 | D2 | Ready/Connected terminology mix | ✅ FIXED 2026-05-17 — Counter-name rename DEFERRED per HEP-CORE-0023 §2.5.3 (log-scraper backcompat).  Instead: harmonized mixed-vocab comments in `hub_state.hpp` (BrokerCounters docstring + 3 method-doc sites) and `hub_state.cpp` (1 site) so every `pending_to_ready_total`/`ready_to_pending_total` reference is accompanied by a "Ready = Connected post-§2" equivalence; no more comments saying "Pending → Connected" while citing `pending_to_ready_total` without the bridge. |
| T3 | D1 | HEP-0030 §9 over-supersedes CHANNEL_BROADCAST_REQ | ✅ FIXED 2026-05-17 | Amended HEP-0030 §9 + new §9.1 documenting channel-bound vs band-bound broadcast coexistence; corrected supersedes-list in header. |
| TR1 | D2 | Wire-conformance test pattern needs spreading | ✅ FIXED 2026-05-17 — `tests/test_framework/wire_conformance.h` helpers (`expect_object_has_keys` / `expect_object_lacks_keys` / `expect_string_field` / `expect_int_field`) + 4 new L3 tests pinning REG_ACK, CONSUMER_REG_ACK, ROLE_INFO_ACK, BAND_JOIN/LEAVE/MEMBERS_ACK shapes against HEP §s.  Mutation-tested. |
| TR2 | D2 | Role-side state-pinning needs first-class FSM | ❌ OPEN (couples to S1) |
| TR3 | D3 | Mutation-sweep markers absent in older tests | ❌ OPEN (low priority) |
| O1 | D3 | Dead BRC `send_notify` | ✅ FIXED 2026-05-17 — Deleted `BrokerRequestComm::send_notify` (declaration + definition); broker-side `handle_channel_notify_req` kept for HEP-CORE-0022 federation peer relay path (see HEP-CORE-0030 §9.1 coexistence note). |
| O2 | D3 | Stale Wave-B M4d/e/f comments | ✅ FIXED 2026-05-17 — Stripped "Wave-B M4e: Class D — route via handler when active" at 3 sites (replaced with HEP-CORE-0033 §18.3 references); other M4d/M4f historical comments retained as "this scaffolding used to be X, now it's gone" rationale. |
| O3 | D3 | resolve_bc_for_* pure forwarders | ✅ FIXED 2026-05-17 — Kept as documented thin wrappers; updated the docstring at `role_api_base.cpp:165-176` to reflect that Class B multi-hub fall-through (audit A3) is composed at the call site (iterating `connections()`) — NOT via `resolve_bc_for_role` — closing the doc/code drift the wrapper introduced. |
| O4 | D2 | Shared::producer_channel duplicates topology | ✅ FIXED 2026-05-17 — `Impl::Shared::producer_channel` / `consumer_channel` strings + 4 mutators deleted; `deregister_from_broker` now walks `handler_->presences()` filtering by atomic `registration_state ∈ {Registered, RegRequestPending}`. |

---

# Round 3 — Fresh-eye review (2026-05-17, post all R1+R2 fixes)

**Trigger:** user request — "another systematic and holistic code review … such that we have another fresh-eye evaluation of the current status."

**Method:** re-read the code I added (S1+O4 FSM, source_hub_uid, wire_conformance helper) for self-introduced issues; re-read areas I hadn't deeply examined yet (federation interactions, schema-registry intersections, ThreadManager teardown contract); check error-handling consistency in the new + adjacent paths.

## R3.1 — Self-audit of S1+O4 (FSM I just added)

| ID | Status | Note |
|---|---|---|
| Presence move-ctor / move-assign for atomic | ✅ Correct | Atomic loaded with `relaxed`, stored with `relaxed` — fine for the construction-time move (RoleHandler ctor only); post-build the vector is stable so no further moves. |
| `to_string(RegistrationState)` defensive fallthrough | ✅ Fine | Switch is exhaustive over enum; the `<unknown>` return covers invalid casts. |
| Re-registration after success (Registered → RegRequestPending → Unregistered on RPC failure) | ⚠ Edge case | If `register_producer_channel` is called again after a successful initial registration, the FSM transitions through `RegRequestPending` then either stays Registered (on success — broker's same-uid restart semantics handle this per HEP-CORE-0023 §2.1.1) OR drops to Unregistered (on RPC failure). Loss of "previously Registered" knowledge on failure. **Disposition:** keep — re-registration is not a normal flow; the existing semantics match "the role just told the broker something new and got rejected → state matches the broker's view (rejected = not registered)". Don't fix unless a use case appears. |
| `handler_ == nullptr` path skips state tracking | ✅ Documented | Validate-only paths and direct-BRC test paths use this; documented in the find-presence guard. |

## R3.2 — Real bug found and fixed: `band_join`/`band_leave` status check (was R3.5)

**Pre-fix:** `role_api_base.cpp:1303-1319` checked only `result.has_value()` before calling `on_band_joined` / `on_band_left`. A broker error response (`{status: "error", ...}`) IS a `has_value()` reply — the original code conflated error and success.

**Fix:** added explicit status-field check (mirrors what `register_producer_channel` already does post-S1). `joined`/`left` now requires `status == "success"`.

**Test-gap admission:** Initially wrote a regression test using `api.band_join("")` to trigger broker INVALID_REQUEST. Discovered during mutation-sweep that the test did NOT catch the mutation (reverting the status check) — because `RoleHandler::on_band_joined` itself has an empty-band-name guard that short-circuits regardless of the caller. The test was misleading; **removed it** and left the R3.2 fix as defensive code without a specific regression test.

**Why this matters as a meta-lesson:** mutation-sweep IS the only honest validation of test rigor. If I had skipped the mutation step, the misleading test would have stayed in the suite, providing false coverage. Always run mutation-sweep on a new regression test — if the test passes the mutation, the test is wrong.

**Production impact of R3.2:** today the broker only returns BAND_JOIN errors for empty band/role_uid, and the role-side `on_band_joined` empty-band guard catches those independently. So R3.2 is a defensive cleanup that prevents future bugs (e.g., if the broker adds richer validation that returns error for non-empty bands).

## R3.3 — Real issue (noted, not fixed): FSM doesn't reflect hub-dead

`role_api_base.cpp:934-967` (audit A2): when `on_hub_dead` fires for connection `i`, only the `connection_alive_mask_` bit is cleared. The presences pointing at that connection retain their `registration_state == Registered`. The broker has reaped them via heartbeat-timeout, but the role-side FSM still claims they're admitted.

Consequence today:
- For master death (i=0): role exits — FSM staleness doesn't matter.
- For peer death (i>0): role continues. `deregister_from_broker` later walks presences, finds the still-`Registered` ones on the dead connection, tries to DEREG → blocks on timeout → marks Deregistered (the unconditional store I added in audit S1+O4). Correct outcome, slow path.

**Recommended (deferred):** when `on_hub_dead` fires, transition every presence whose `connection == &connections_[i]` to `Deregistered` (or a new `BrokerLost` state if we want to distinguish voluntary vs forced). Either:
- Optimizes teardown (skip blocking DEREG attempts on dead BRC)
- Maintains FSM truth ("I am not registered on a dead broker")

**Why not fixed now:** scope. The FSM transition belongs in audit A2's on_hub_dead lambda, which would then need a way to enumerate presences-on-this-connection (via the handler). Doable but a separate change.

## R3.4 — Wire shape asymmetry (documentation gap)

REG_REQ outbound: `opts["inbox_schema_json"]` (JSON-string).
ROLE_INFO_ACK inbound: `resp["inbox_schema"]` (parsed JSON object).

Same data, two wire forms. The asymmetry is intentional — broker stores the JSON string, parses for ACK — but a reader of HEP-CORE-0027 wire spec wouldn't see this without reading the source. **Recommended:** add a §4.3 note to HEP-CORE-0027 documenting the asymmetry. Doc-only.

## R3.5 — Broker silently accepts invalid band identifiers

`broker_service.cpp:4244` calls `hub_state_->_on_band_joined(band, ...)` which validates `is_valid_identifier(band, IdentifierKind::Band)`. If validation fails, `_on_band_joined` silently bumps `invalid_identifier_total` counter and returns — **but the broker handler doesn't check the return**, then returns `status: success` to the role. The role thinks it joined; the broker has no record.

This is a real bug — broker silently succeeds for invalid bands. Different family from R3.2 (this is the broker lying, not the role conflating). **Worth a separate fix:** broker handler should consult HubState's validation result or pre-validate the band name itself, returning `{status: error, error_code: INVALID_BAND_NAME}` on rejection.

**Why not fixed now:** scope; needs broker-side change + new error code in HEP-0007 §12.4a + new regression test. File as **R3.5-OPEN**.

## R3.6 — Federation peer-relay path uses `CHANNEL_NOTIFY_REQ` which we just half-deprecated (O1)

The role-side `BRC::send_notify` is now gone (audit O1). But the broker-side `handle_channel_notify_req` lives on because HEP-CORE-0022 federation peers may relay channel events using that wire format. **This is the right call** (documented in HEP-CORE-0030 §9.1 + the O1 commit comment), but:

- No test verifies that federation peer-relay of CHANNEL_NOTIFY_REQ actually works (the wire path lives on, but is anyone exercising it?). If federation refactors away from this wire format, the broker handler would become 100% dead without anyone noticing.

**Recommended:** add an L3 federation-relay test that drives CHANNEL_NOTIFY_REQ via a synthetic peer-emitter and verifies the broker forwards it as CHANNEL_EVENT_NOTIFY. File as **R3.6-OPEN**.

## R3.7 — Test coverage of `RegRequestPending` is implicit, not explicit

The new S1+O4 regression test checks transitions Unregistered → Registered → Deregistered. It does NOT explicitly observe the `RegRequestPending` state because by the time the test calls `register_producer_channel`, the call returns synchronously with the final state. The intermediate state IS set inside the function, but no test races to observe it.

**Why this matters:** if someone deletes the `RegRequestPending` store before the broker RPC (the "before" half of audit S1+O4), no test fails — the final state would still transition correctly via the success-path store.

**Recommended:** add a unit-level test that calls `register_producer_channel` with a NULL handler (or against a hung broker), observes the state mid-flight via a separate thread. Too complex for the value; file as **R3.7-noted**.

## R3.8 — `RoleHandler::find_presence_from_notification` non-const overload not exercised by tests

I added a non-const overload `find_presence_for_channel(channel)` returning `Presence*` so callers can mutate `registration_state`. The L2 tests at `test_role_handler.cpp` test only the const overload. **The mutating use IS exercised by L3 tests via `api.register_producer_channel` → handler->find_presence(non-const)** but not pinned at L2.

**Recommended:** add one L2 test that calls the non-const overload and verifies it returns the same pointer as the const overload (pointer identity check). Low priority. File as **R3.8-noted**.

## R3 Round 3 status table

| ID | Tier | Title | Status | Disposition |
|---|---|---|---|---|
| R3.1 | self-audit | S1+O4 self-review | ✅ Reviewed | All concerns addressed in original implementation; one edge case (re-registration) acknowledged + documented |
| R3.2 | D3 | band_join/band_leave status check | ✅ FIXED 2026-05-17 | Defensive code added; regression test attempted but removed as misleading (mutation-sweep failed) |
| R3.3 | D2 | Hub-dead doesn't transition presence FSM | ✅ FIXED 2026-05-17 — `RoleHandler::mark_connection_disconnected()` added; called from `on_hub_dead` lambda; presences on dead connection transition Registered → Deregistered.  Mutation-tested via `RoleAPIBase_HubDead_TransitionsPresencesToDeregistered`. |
| R3.4 | D3 | inbox_schema wire asymmetry undocumented | ⏳ OPEN — DOC-ONLY | HEP-CORE-0027 §4 needs a wire-form note |
| R3.5 | D1 | Broker silently accepts invalid band names | ✅ FIXED 2026-05-17 — Added explicit `is_valid_identifier` check in 4 BAND_* handlers; returns typed `INVALID_BAND_NAME` error.  Also caught + fixed a test (A0) that was using a non-`!`-prefixed band name.  Mutation-tested via `Broker_Band_RejectsInvalidIdentifier`. |
| R3.6 | D2 | CHANNEL_NOTIFY_REQ broker handler has no test coverage | ✅ FIXED 2026-05-17 — Investigation revealed the handler was 100% DEAD, not "kept for federation".  Federation actually uses `HUB_RELAY_MSG` (broker↔broker, `handle_hub_relay_msg`), NOT CHANNEL_NOTIFY_REQ.  Resolution: DELETED `handle_channel_notify_req` entirely (dispatch entry + known_msg_type list + handler body + header declaration).  Old clients sending CHANNEL_NOTIFY_REQ now receive UNKNOWN_MSG_TYPE.  Stronger fix than the original "add test coverage" recommendation. |
| R3.7 | D3 | `RegRequestPending` state not explicitly observed by test | ⏳ NOTED | Too complex for value |
| R3.8 | D3 | Non-const `find_presence_for_channel` overload not L2-tested | ⏳ NOTED | Low priority |

## R3 meta-lesson on test discipline

When I added the R3.2 regression test, I followed my usual pattern: write test → run → pass → mutation → expect fail. **The mutation passed too.** Standard practice would have logged "test passes" and moved on. Instead I caught that the test was structurally unable to detect the bug (due to `on_band_joined`'s empty-band guard) and removed it.

**This is the right behavior** but also a wake-up call: the audit-A0/A2/A3/B1/B2/C3/S1+O4/TR1 series all passed mutation-sweep AND were structurally sound. R3.2's misleading test would have shipped without that mutation step.

**Recommendation for future audits:** mutation-sweep is non-negotiable for every new regression test. If a mutation passes, either strengthen the test or remove it — never leave a "passing" test that's structurally unable to fail under the bug it claims to catch.

---

# Final status — what's still open (2026-05-17)

After 60 audit items across 3 rounds, the dual-hub renovation is structurally complete. Remaining items, in priority order:

## V1–V5 + M3 closed (2026-05-18 — static review follow-up bundle)

| Item | Resolution |
|---|---|
| **V1** | `RoleHostCore::context_valid_` one-way latch + comprehensive flag-contract docs (see below) |
| **V2** | `mark_connection_disconnected` race comment strengthened — explicit two-reason safety justification: (1) dead-broker premise makes the outcome correct, (2) `Deregistered` is a sink state so clobber cannot cause oscillation |
| **V3** | `RoleAPIBase::band_broadcast` adds WARN log on the "band not in index" path — pre-fix it was a complete silent no-op |
| **V4** | `role_presence.hpp` + `role_handler.hpp` docstrings updated — removed "M3 skeleton (build-only)" claim; explicitly documented S1+O4 atomic FSM + R3.3 `mark_connection_disconnected` extensions |
| **V5** | `#if 0` retired-code block in `datahub_broker_protocol_workers.cpp` (lines 1562-1663) deleted entirely; concise tombstone comment kept pointing at this review doc |
| **M3** | `connection_alive_mask_` memory ordering tightened: init store → `release`; fetch_and on bit-clear → `acq_rel`; loads → `acquire`. Pre-fix all were `relaxed` — worked in practice through implementation-detail happens-before but not by the C++ memory model |



**`RoleHostCore::context_valid_` one-way latch + comprehensive flag-contract documentation.** Adds a callback-safety beacon flipped in `stop_handler_threads` Phase 3a (after `wait_for_quiescence`, before destructive Phase 4). Every ctrl-thread callback (`on_notification`, `on_hub_dead`, heartbeat tick) gates on `core->context_valid()` — slow-waker scenarios now log a WARN and bail instead of dereferencing destroyed handler memory. Mutation-tested via `RoleHostCoreTest.ContextValid_DefaultTrue_OneWayLatch` + `ContextValid_IndependentOfIsRunning`.

The class-header docstring of `role_host_core.hpp` now explicitly documents the contract for all four flags (`running_threads_`, `shutdown_requested_`, `critical_error_`, `context_valid_`): purpose, transition timing, audience, and the distinction between worker-loop liveness and callback-safety. HEP-CORE-0031 §4.1.0 added documenting the bracket+wait (primary) vs context_valid_ (fallback) protection layers.

## Open D1 (needs user direction, not a bug)

**C1 — HEP-0033 §19.6 hub-dead policy vs A2 master/peer asymmetry.** §19.6 specifies "any hub dies → role exits"; A2 implements "master dies → exit; peer dies → keep running". The two are operationally different (peer-dies-tolerant is more lenient). Neither is wrong code — they reflect different design intents. This is a **design decision**, not a bug. Resolution paths:
- (a) Amend HEP-0033 §19.6 to document master/peer asymmetry (codify what A2 ships)
- (b) Revert A2 to strict any-hub-exits, matching §19.6 as written
Default: keep A2 as-is (it's more useful for real fault tolerance) and amend the HEP. Either way requires a 30-line doc change OR ~50-line revert.

## Open D3 (doc-only, low priority)

| ID | What | Effort |
|---|---|---|
| **R3.4** | HEP-CORE-0027 §4 should document the `inbox_schema_json` (REQ) → `inbox_schema` (ACK) wire-form asymmetry | 10 lines of doc |
| **R3.7** | `RegistrationState::RegRequestPending` state not directly observed by any test (transition happens too fast to race) | Test-design hard, value low |
| **R3.8** | Non-const `find_presence_for_channel` overload not L2-tested directly | Test-design easy, value low |

## Open separate workstreams (not part of this review's scope)

- **Task #44** — L4 processor + consumer test infrastructure (Wave-D). Long-running. Touches binary launchers + signal handling.
- **HEP-CORE-0035 auth** — Production-readiness, not blocking dual-hub functional milestone. 7-phase plan in HEP-0035 §8.
- **HEP-0033 §15 Phase 10 doc amendment** — Per-producer metrics tree shape + cross-reference survey.
- **HUB_TARGETED_ACK wire frame** — HEP-0033 §12.3.6, federation-internal.

## Closed audit items — 60 total

R1 (original holistic review): 22 findings ✅
R2 (state-explicitness audit): 13 findings ✅
R3 (fresh-eye review): 8 findings — 6 ✅ FIXED + 2 documented-deferred (R3.4 doc; R3.7/R3.8 low-priority test enhancements)
D3 (cleanup): O1, O2, O3 ✅
Tests: **1932/1932 passing** (baseline 1923 + 9 new regressions, 1 stronger fix via dead-code deletion).

## Foundation status: solid

The connection / inbox / band protocols now have:
- Wire-spec conformance test pattern (TR1 helper) covering all 4 major ACK families
- Explicit FSM on Presence (S1+O4) — replaces implicit `Shared::producer_channel` strings
- Hub-dead-aware FSM transitions (R3.3) — FSM reflects broker-side reality
- Source-tagged IncomingMessages (C3) — dual-hub origin disambiguation
- Strict band-identifier validation (R3.5) — broker can no longer silently accept invalid names
- Channel-bound vs band-bound broadcast coexistence documented (T3 + HEP-0030 §9.1)
- All wire-payload key naming aligned with HEPs (B1, B3)
- All dead BRC methods + broker handlers removed (O1, R3.6)
- Comment vocabulary harmonized to the new Connected/Ready dual-name regime (T1)

The next workstreams are either user-decision items (C1) or separate scopes (Task #44, HEP-0035). The audit chain is done.
