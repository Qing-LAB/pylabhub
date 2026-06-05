# Query Layer Migration TODO

**Scope:** Concrete migration sites for the inline-join hotspots
catalogued during the 2026-06-02 survey.  Patterns + canonical
helpers are documented in `docs/HEP/HEP-CORE-0039-Hub-State-Query-Layer.md`;
this file lists the SITES.

**Source of truth for status:** `docs/TODO_MASTER.md` (when HEP-0039
work is active).

**Completed-work archive:** `docs/archive/transient-2026-06-05/todo-completions/QUERY_LAYER_TODO_completions.md`
(Pattern P8 — full migration shape + prerequisites + Step A/B
commits).

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

## Pattern P8 — Heartbeat-timeout sweep — CLOSED

Migrated to `for_each_presence_matching` in two steps (Pass-1 +
Pass-2) including the `Pass2Decision` struct + per-channel
visit/apply + `channel_torn_down` short-circuit + two-snapshot
invariant.  Full migration shape + prerequisites + behavior-
preservation notes preserved verbatim in
`docs/archive/transient-2026-06-05/todo-completions/QUERY_LAYER_TODO_completions.md`.

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

---

**Tracking:** The HEP-0039 implementation work itself (Phases A-D)
should be a TODO_MASTER.md sprint entry.  The migration pattern
sweeps (Phase E) are open-ended and tracked here per-pattern.
