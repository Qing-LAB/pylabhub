# Query Layer Migration TODO

**Scope:** Concrete migration sites for the inline-join hotspots
catalogued during the 2026-06-02 survey.  Patterns + canonical
helpers are documented in `docs/HEP/HEP-CORE-0039-Hub-State-Query-Layer.md`;
this file lists the SITES.

**Source of truth for status:** `docs/TODO_MASTER.md` (when HEP-0039
work is active).

**Picking order:** highest duplication first.  Pattern P1 is the
densest (7 sites + the heartbeat-timeout sweep on top).  Migrate
one pattern at a time; each migration is its own commit with the
behavior-preservation regression bar (see HEP-0039 §10.4).

---

## Pattern P1 — `channel × producer × producer-presence` triple-join

**Canonical helper:** `compute_channel_observable(ch, roles_map)` (for the
observable reduction); `for_each_producer_with_presence(ch, roles, fn)`
(for arbitrary reductions); `enumerate_live_producers(ch, roles)` (for
"give me kLive producers" specifically); `is_producer_live(ch, uid, roles)`
(for the predicate form).

**Sites** (open-coded today; should migrate to the helper that fits each
reduction):

| File:line | Reduction | Suggested helper |
|---|---|---|
| `src/utils/ipc/broker_service.cpp:1692-1707` | DISC_REQ: pick "first Live, else first non-Disconnected" | `for_each_producer_with_presence` |
| `src/utils/ipc/broker_service.cpp:2252-2265` | HEARTBEAT_REQ: was-pending boolean | `compute_channel_observable(...) != kLive` |
| `src/utils/ipc/broker_service.cpp:2897-2919` | Heartbeat-timeout pass-1: Connected→Pending | `for_each_presence_matching` (sweep visitor) |
| `src/utils/ipc/broker_service.cpp:2961-3006` | Heartbeat-timeout pass-2: Pending→Disconnected | `for_each_presence_matching` |
| `src/utils/ipc/hub_state.cpp:225-236` | Channel observable in snapshot path | THIS IS the canonical site (`compute_channel_observable`) — no migration needed; serves as reference |
| `src/utils/ipc/hub_state.cpp:839` | Cascade scan in `_on_channel_closed` | `for_each_producer_with_presence` |
| `src/utils/ipc/hub_state.cpp:1143-1167` | `channel_metrics_snapshot` aggregator | `for_each_producer_with_presence` |

**Notes:** The first site (`hub_state.hpp:1142-1155`,
`compute_channel_observable` itself) IS the canonical reference
implementation of this join.  All other sites should call into the
new helpers (which share its templated-on-`RolesMap` strategy).

## Pattern P2 — `channel × consumer × consumer-presence` triple-join

**Canonical helper:** `for_each_consumer_with_presence(ch, roles, fn)`
(new — no canonical exists today).

**Sites:**

| File:line | Reduction | Suggested helper |
|---|---|---|
| `src/utils/ipc/broker_service.cpp:2926-2940` | Heartbeat-timeout pass-1: consumer Connected→Pending | `for_each_presence_matching` |
| `src/utils/ipc/broker_service.cpp:3014-3036` | Heartbeat-timeout pass-2: consumer Pending→Disconnected | `for_each_presence_matching` |
| `src/utils/ipc/hub_state.cpp:238` | Cascade scan, consumer side | `for_each_consumer_with_presence` |

## Pattern P3 — "Find role across the hub by uid"

**Canonical helper:** `find_role_attachments(snap, uid) ->
std::vector<RoleAttachment>` (new — no canonical exists today;
returns ALL attachments because a role may appear as producer on
one channel AND consumer on another per HEP-0023 §2.1 + HEP-0033 §19).

**Sites:**

| File:line | Use | Migration |
|---|---|---|
| `src/utils/ipc/broker_service.cpp:3453-3482` | ROLE_PRESENCE_REQ: which channel hosts this uid? | Replace with `find_role_attachments` + JSON-build over all attachments |
| `src/utils/ipc/broker_service.cpp:3520-3562` | ROLE_INFO_REQ: producer side | `find_role_attachments` (filter by `role_type == "producer"`) |
| `src/utils/ipc/broker_service.cpp:3565-3608` | ROLE_INFO_REQ: consumer side | `find_role_attachments` (filter by `role_type == "consumer"`) |

## Pattern P4 — Filter `consumers[]` by `uid` (`find_consumer` exists, not used)

**Canonical helper:** `ChannelEntry::find_consumer(uid)` — ALREADY EXISTS.
This is a "use what's there" cleanup; no new code, just replace inline
walks with the call.

**Sites:**

| File:line | Use |
|---|---|
| `src/utils/ipc/broker_service.cpp:3467-3480` | ROLE_PRESENCE_REQ consumer branch — inline walk |
| `src/utils/ipc/broker_service.cpp:3567-3606` | ROLE_INFO_REQ consumer branch — inline walk |

## Pattern P5 — NOTIFY fan-out (walk parties, skip empty identity, send)

**Canonical helper:** `for_each_party_identity(ch, kind, fn)` (new).

**Sites:**

| File:line | NOTIFY | Party kind |
|---|---|---|
| `src/utils/ipc/broker_service.cpp:3054-3062` | CONSUMER_DIED_NOTIFY (from pending-timeout consumer path) | Producer |
| `src/utils/ipc/broker_service.cpp:3109-3117` | CONSUMER_DIED_NOTIFY (Cat-2 PID-dead detection) | Producer |
| `src/utils/ipc/broker_service.cpp:3134-3151` | CHANNEL_CLOSING_NOTIFY | Consumer |
| `src/utils/ipc/broker_service.cpp:3154-3169` | CHANNEL_CLOSING_NOTIFY | Producer |
| (likely 1+ more — re-scan for `send_to_identity` + `if (identity.empty()) continue;` pattern) | other NOTIFY paths | — |

## Pattern P6 — Collect `role_uid`s into `std::vector<std::string>`

**Canonical helpers:** `producer_uids(ch)` and `consumer_uids(ch)` (new).

**Sites:**

| File:line | Use |
|---|---|
| `src/utils/ipc/hub_state.cpp:1142-1148` | `_on_channel_closed` cascade collector |
| `src/utils/ipc/broker_service.cpp:3399` | JSON list-channels: producer uids |
| `src/utils/ipc/broker_service.cpp:3736-3738, 3770-3775` | `list_channels_json_str` / `query_channel_snapshot`: producer uids + pids pair |

(The "uids + pids" pair sites may want a `(uid, pid)` pair-extractor in
addition to `producer_uids` — to be decided during Phase A.)

## Pattern P7 — "Is producer P kLive on channel X?" inline state check

**Canonical helper:** `is_producer_live(ch, uid, roles)` (new).

**Sites:** Recomputed inline in the same triple-join blocks as P1.  Where
the surrounding context is a per-iteration predicate (DISC, sweeps), use
the helper directly instead of replicating the
`state == Connected && first_heartbeat_seen` check.

## Pattern P8 — Heartbeat-timeout sweep (the densest single block)

**Canonical helper:** `for_each_presence_matching(ch, roles, predicate, fn)`
(new sweep visitor).

**Sites:** ONE block — `check_heartbeat_timeouts` in
`src/utils/ipc/broker_service.cpp` spans **lines 2879-3068, ~190 lines**.
Four near-identical nested loops:

1. Pass-1 producer Connected→Pending (lines 2897-2919)
2. Pass-1 consumer Connected→Pending (lines 2926-2940)
3. Pass-2 producer Pending→Disconnected (lines 2961-3006)
4. Pass-2 consumer Pending→Disconnected (lines 3014-3066)

**This is the proof-of-use migration site for the framework.**  Per
HEP-0039 §9 Phase D, the sweep migration is performed in the same
work stream as the framework code lands.

**Migration shape — two snapshots, two passes, collect-then-apply.**
The existing code already uses two snapshots (`snap` at line 2889 +
`snap2` at line 2957) because Pass-2 must observe Pass-1's
mutations (specifically the fresh `state_since` Pass-1 stamps on
Connected→Pending transitions; this excludes just-demoted presences
from same-sweep termination — load-bearing behavior).  The
migration MUST preserve this:

```cpp
// Broker-side decision types — local to the sweep.
struct Pass1Decision {
    std::string channel;
    std::string role_uid;
    std::string role_type;   // "producer" | "consumer"
};
struct Pass2Decision {
    PartyKind                       party;
    std::string                     channel;
    std::string                     role_uid;
    ChannelEntry                    pre_drop_channel;   // value copy
    std::optional<ConsumerEntry>    pre_drop_consumer;  // populated on consumer path
};

// ── Pass-1 (Connected → Pending) ────────────────────────────────
auto snap1 = hub_state_->snapshot();
std::vector<Pass1Decision> p1;
for (const auto &[name, ch] : snap1.channels) {
    for_each_presence_matching(
        ch, snap1.roles,
        [&](const RolePresence &p) {
            return p.state == RoleState::Connected
                && (now - p.last_heartbeat) >= ready_timeout;
        },
        [&](const PresenceSweepTarget &t) {
            const std::string uid = (t.party == PartyKind::Producer
                                      ? t.producer->role_uid
                                      : t.consumer->role_uid);
            const std::string role_type = (t.party == PartyKind::Producer
                                            ? "producer" : "consumer");
            p1.push_back({name, uid, role_type});
        });
}
// Apply Pass-1.  _on_heartbeat_timeout takes the writer lock per
// call; counter bumps + ChannelStatusChangedHandler (producer only)
// fire from inside.
for (const auto &d : p1)
    hub_state_->_on_heartbeat_timeout(d.channel, d.uid, d.role_type);

// ── Pass-2 (Pending → Disconnected) ─────────────────────────────
auto snap2 = hub_state_->snapshot();   // FRESH — observes Pass-1 mutations
std::vector<Pass2Decision> p2;
for (const auto &[name, ch] : snap2.channels) {
    for_each_presence_matching(
        ch, snap2.roles,
        [&](const RolePresence &p) {
            return p.state == RoleState::Pending
                && (now - p.state_since) >= pending_timeout;
        },
        [&](const PresenceSweepTarget &t) {
            // Capture pre_drop NOW — the apply phase may erase
            // the channel from live state via _on_channel_closed.
            Pass2Decision d;
            d.party             = t.party;
            d.channel           = t.channel;
            d.pre_drop_channel  = *t.channel_entry;  // value copy
            if (t.party == PartyKind::Producer) {
                d.role_uid = t.producer->role_uid;
            } else {
                d.role_uid          = t.consumer->role_uid;
                d.pre_drop_consumer = *t.consumer;   // value copy
            }
            p2.push_back(std::move(d));
        });
}
// Apply Pass-2.  Preserve the original sweep's ordering:
// producers within a channel before consumers in the same channel,
// AND honor the channel_torn_down short-circuit (once a producer
// teardown evicts the channel, skip Pass-2 consumer iteration on
// that channel — CONSUMER_DIED_NOTIFY to a vanished channel is wrong).
std::unordered_set<std::string> torn_down_channels;
for (const auto &d : p2) {
    if (d.party == PartyKind::Producer) {
        auto drop = hub_state_->_on_pending_timeout(
            d.channel, d.role_uid, "producer");
        if (drop.removed && drop.channel_now_empty) {
            send_closing_notify(socket, d.channel, d.pre_drop_channel,
                                "pending_timeout");
            on_channel_closed(socket, d.channel, d.pre_drop_channel,
                              "pending_timeout");
            torn_down_channels.insert(d.channel);
        }
    } else {
        if (torn_down_channels.count(d.channel)) continue;
        auto drop = hub_state_->_on_pending_timeout(
            d.channel, d.role_uid, "consumer");
        if (drop.removed) {
            // CONSUMER_DIED_NOTIFY body shape inlined to match
            // production at `broker_service.cpp:3048-3053` exactly
            // (broker_proto 4→5 — fields: channel_name, role_uid,
            // consumer_pid, consumer_hostname, reason).  A future
            // refactor MAY extract this into a helper as part of
            // the migration; the inline form here is the canonical
            // contract.
            nlohmann::json notify;
            notify["channel_name"]      = d.channel;
            notify["role_uid"]          = d.pre_drop_consumer->role_uid;
            notify["consumer_pid"]      = d.pre_drop_consumer->consumer_pid;
            notify["consumer_hostname"] = d.pre_drop_consumer->consumer_hostname;
            notify["reason"]            = "heartbeat_timeout";
            for_each_party_identity(
                d.pre_drop_channel, PartyKind::Producer,
                [&](std::string_view id, std::string_view /*uid*/) {
                    send_to_identity(socket, std::string(id),
                                     "CONSUMER_DIED_NOTIFY", notify);
                });
            on_consumer_closed(socket, d.channel,
                               *d.pre_drop_consumer, "heartbeat_timeout");
        }
    }
}
```

Expected diff: ~190 lines → ~80 lines (the broker-side
`Pass2Decision` struct + the apply-phase ordering logic add weight
the original loops had implicitly).  Behavior preserved including:

- two snapshots (Pass-1's fresh `state_since` excludes from Pass-2)
- `pre_drop` captured before the mutator runs
- `channel_torn_down` short-circuit between producer and consumer
  Pass-2 within the same channel
- `CONSUMER_DIED_NOTIFY` body shape including `reason="heartbeat_timeout"`
- producer-only `ChannelStatusChangedHandler` fan-out (fires from
  inside `_on_heartbeat_timeout` for `role_type=="producer"`)

**See HEP-CORE-0039 §3.2a "Lock discipline" + §6 "Two-passes-with-
cross-pass-dependency note" for the rationale.**

**Migration prerequisites** (must land before the migration commit):

1. ✅ Framework doc-comment corrected (`hub_state_queries.hpp`
   `for_each_presence_matching` lock-discipline paragraph)
2. ✅ `PresenceSweepTarget::channel_entry` field added (consumer
   branch covered by L2 test `ConsumerBranch_TargetFullyPopulated`)
3. ✅ HEP §6 cross-pass-dependency note added
4. ✅ L3 `test_datahub_broker_health.cpp` strengthened with:
   - Multi-producer partial pending-timeout (drop one, channel
     survives, counter exact)
   - Consumer-pending-timeout `CONSUMER_DIED_NOTIFY` body shape
     including `reason="heartbeat_timeout"` and exact PID pinning
   - Pass-1 demotion in tick T is NOT followed by Pass-2 termination
     in same tick T (the two-snapshot invariant)
   - `channel_torn_down` short-circuit: producer + consumer both
     Pending same tick → producer Pass-2 evicts channel → no stray
     `CONSUMER_DIED_NOTIFY` (test:
     `ChannelTornDown_ConsumerPass2Skipped`)
5. ⏳ Migration itself, in two commits:
   - Step A: Pass-1 only (no fan-out, no pre_drop, no cascade)
   - Step B: Pass-2 with the `Pass2Decision` struct + ordering logic

## Pattern P9 — Mutator-side joins (OUT OF SCOPE for HEP-0039)

These are mutators, not queries.  They are governed by HEP-CORE-0033
"Hub State Mutation" and the Core Structure Change Protocol in
`docs/IMPLEMENTATION_GUIDANCE.md`.  Cataloged here only so future
mutator-cleanup work has the inventory.

This corresponds to the single "Mutator-side joins" row in HEP-0039
§6, which folds the two mutator patterns the survey found (dereg
triple + ChannelStatusChangedHandler refire preamble) into one
out-of-scope row.

| File:line | Pattern | Out-of-scope reason |
|---|---|---|
| `src/utils/ipc/hub_state.cpp:1085-1091`, `:1156-1166`, `:1169-1173`, `:1245-1249` | `roles.find(uid) + on_dereg + drop_channel_if_orphaned` | Mutator; HEP-0033 owns this |
| `src/utils/ipc/hub_state.cpp:1322-1330`, `:1380-1386` | `ChannelStatusChangedHandler` refire preamble | Mutator side-effect; HEP-0033 owns this |
| `src/utils/ipc/hub_state.cpp:1427-1444`, `:1476-1488` | `_on_pending_timeout` per-presence apply | Mutator |

## Sites that already use the canonical helpers correctly (reference)

These do NOT need migration; they're listed here so reviewers know
what "correct usage" looks like.

| File:line | Helper used | Notes |
|---|---|---|
| `src/utils/service/hub_api.cpp:243, 263, 276, 296, 316` | `HubState::snapshot()` + `list_*` / `get_*` | Already snapshot-coherent per request; will become one-line Layer 2b wrappers in Phase B |
| `src/utils/ipc/admin_service.cpp:372, 395, 412, 438, 447` | Same | Same |
| `src/utils/ipc/hub_state_json.cpp:60-86` | `channel_to_json`, `role_to_json`, etc. | Canonical serializer; not in scope for migration |
| `src/utils/ipc/hub_state.hpp:1142-1155` | `compute_channel_observable` | THE reference impl of the templated-on-RolesMap pattern |
| `src/utils/ipc/hub_state.cpp:208-251` (`channel_metrics_snapshot`) | Walks under the same triple-join as P1 but does NOT migrate — it lives in `HubState` and is itself a canonical query | Note: per-pattern review may identify common pieces to refactor here too |

## Outdated types flagged for retirement

These are listed here so future cleanup is tracked; retirement itself
is HEP-0039 Phase E (open-ended cleanup).

| Type | Where | Reason | Retire when |
|---|---|---|---|
| `ChannelSnapshotEntry`, `ChannelSnapshot` | `src/include/utils/broker_service.hpp:58, 72` | Only used by L3 worker tests; Layer 2b `nlohmann::json` covers the case | After L3 worker tests migrate (each one its own task) |
| `RoleStateMetrics` | `src/include/utils/broker_service.hpp:91` | Strict subset of `BrokerCounters`; exists for test-stable shape only | Same |
| `BrokerService::query_channel_snapshot()` | `broker_service.cpp:3754` | Wraps `HubState::snapshot()` + `channel_to_json` | After call sites migrate |
| `BrokerService::query_shm_info` / `collect_shm_info_json` | `broker_service.cpp:481` | Subsumed by `list_shm_blocks(snap)` / `get_shm_block(snap, channel)` | After call sites migrate |

## Recent Completions

(none yet — this TODO created 2026-06-02 with the HEP)

---

**Tracking:** The HEP-0039 implementation work itself (Phases A-D)
should be a TODO_MASTER.md sprint entry.  The migration pattern
sweeps (Phase E) are open-ended and tracked here per-pattern.
