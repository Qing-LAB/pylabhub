# QUERY_LAYER_TODO completions archive — 2026-06-05

This file preserves verbatim prose for QUERY_LAYER_TODO entries that were
verified shipped in code as of 2026-06-05.  Moved here so the active
QUERY_LAYER_TODO can focus on open patterns (P1–P7, P9).

Source file: `docs/todo/QUERY_LAYER_TODO.md`.

---

## Pattern P8 — Heartbeat-timeout sweep (CLOSED)

The densest single block in the original survey: `check_heartbeat_timeouts`
in `src/utils/ipc/broker_service.cpp` spans lines 2879-3068, ~190 lines,
four near-identical nested loops:

1. Pass-1 producer Connected→Pending (lines 2897-2919)
2. Pass-1 consumer Connected→Pending (lines 2926-2940)
3. Pass-2 producer Pending→Disconnected (lines 2961-3006)
4. Pass-2 consumer Pending→Disconnected (lines 3014-3066)

**This is the proof-of-use migration site for the framework.**  Per
HEP-0039 §9 Phase D, the sweep migration was performed in the same
work stream as the framework code landed.

### Migration shape — two snapshots, two passes, collect-then-apply

The existing code already used two snapshots (`snap` at line 2889 +
`snap2` at line 2957) because Pass-2 must observe Pass-1's mutations
(specifically the fresh `state_since` Pass-1 stamps on
Connected→Pending transitions; this excludes just-demoted presences
from same-sweep termination — load-bearing behavior).  The migration
preserved this:

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

### Migration prerequisites (shipped)

All prerequisites landed before the migration commits:

1. Framework doc-comment corrected (`hub_state_queries.hpp`
   `for_each_presence_matching` lock-discipline paragraph) — task #148
2. `PresenceSweepTarget::channel_entry` field added (consumer branch
   covered by L2 test `ConsumerBranch_TargetFullyPopulated`) — task #145
3. HEP §6 cross-pass-dependency note added — task #146
4. L3 `test_datahub_broker_health.cpp` strengthened with:
   - Multi-producer partial pending-timeout (drop one, channel
     survives, counter exact)
   - Consumer-pending-timeout `CONSUMER_DIED_NOTIFY` body shape
     including `reason="heartbeat_timeout"` and exact PID pinning
   - Pass-1 demotion in tick T is NOT followed by Pass-2 termination
     in same tick T (the two-snapshot invariant)
   - `channel_torn_down` short-circuit: producer + consumer both
     Pending same tick → producer Pass-2 evicts channel → no stray
     `CONSUMER_DIED_NOTIFY` (test:
     `ChannelTornDown_ConsumerPass2Skipped`) — task #147
5. Migration itself, in two commits:
   - Step A: Pass-1 only (commit `9c351c34`, 2026-06-02) — task #149
   - Step B: Pass-2 with the `Pass2Decision` struct + per-channel
     visit/apply + `channel_torn_down` short-circuit (2026-06-02) —
     task #150

---

## Recent Completions log (verbatim from source file)

- **2026-06-02** — P8 Step B: migrated Pass-2 (Pending→Disconnected) of
  `BrokerServiceImpl::check_heartbeat_timeouts` to
  `for_each_presence_matching`.  Per-channel two-phase: visit collects
  `Pass2Decision` over `snap2` (producers then consumers in declaration
  order, with `pre_drop_channel` / `pre_drop_consumer` value-copied from
  the snapshot); apply drains producer decisions first (with
  `channel_torn_down` short-circuit on last-producer atomic teardown),
  then consumer decisions.  Preserves all original log lines,
  `CONSUMER_DIED_NOTIFY` body shape, and the two-snapshot invariant.
  Fresh-eye review returned 1 MED (transient line-number citation in
  comment, fixed in same commit) + 2 LOWs, no behavioral divergence.
- **2026-06-02** — P8 Step A: migrated Pass-1 (Connected→Pending) to
  `for_each_presence_matching` (commit 9c351c34).
