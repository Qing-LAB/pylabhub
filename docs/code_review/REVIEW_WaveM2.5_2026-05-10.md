# Wave M2.5 Audit — Controlled-Access API on `ChannelEntry`

**Date:** 2026-05-10
**Branch:** `feature/lua-role-support` (post `e26bbb6` — M2.5 step 2b shipped)
**Scope:** The work in this session (commits `4e3c68f` → `e26bbb6`), plus a
**broader sweep** of code, comments, and HEP docs for inconsistencies with
the multi-producer ChannelEntry model that the M2.5 design locked. The
goal of this pass is to surface every drift point that step 3 (broker
REG_REQ handler rewrite) would otherwise inherit.

**Tests as of this audit:** 1778/1778 passing (one L4 timing flake passes
on isolated retry).

**Mutation sweep performed:** 6 of 6 load-bearing assertions in the new
`ChannelEntryApi` tests catch their mutations (find_producer uid-conflict
check; SHM cardinality check; find_consumer uid-conflict check; remove_producer
"removed" flag; aggregate_metadata_tree null-omission; set_producer_zmq_node_endpoint
uid match). Sensitivity check passed — no outcome-only assertions.

---

## Findings

### F1 — HEP-CORE-0007 §12.4a missing `UID_CONFLICT` error code  ❌ OPEN

**Severity:** Blocker for step 3 (wire-shape contract).
**Files:**
- `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` §12.4a — error code taxonomy table.

**Problem:** M2.5 design locked the strict reject rule (see
`docs/tech_draft/controlled_access_api_design.md` §6.2): any REG_REQ /
CONSUMER_REG_REQ with a uid that already exists in HubState is rejected
with `UID_CONFLICT`. The protocol HEP has `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM`
already (good) but does NOT have `UID_CONFLICT` listed. Step 3 ships this
wire error code; the HEP must precede or co-land with the code change.

**Worse:** the existing `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM` entry says
"Same-`role_uid` restart is admitted as restart-replace and does not trigger
this error." That **contradicts** the new strict policy — same-uid is now
always rejected with `UID_CONFLICT` (not `MULTI_PRODUCER_*`). The entry must
be reworded.

**Fix in this review:** add `UID_CONFLICT` row to §12.4a; reword the
`MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM` entry to remove the
"restart-replace" wording.

---

### F2 — HEP-CORE-0007 §12.4 DISC_REQ_ACK `metadata` wire shape drift  ❌ OPEN

**Severity:** Blocker for step 3 (wire-shape contract).
**Files:**
- `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md:888` —
  `metadata  object   Channel metadata (free-form JSON object; producer-supplied via REG_REQ).`

**Problem:** Per M2.5 design §6.1, `metadata` is per-producer; channel-level
DISC_REQ_ACK emits a tree keyed by producer `role_uid`. The HEP still
describes the old flat-blob shape. After step 3 lands, the wire response
shape changes; the HEP must already document the new shape so consumers
have a stable reference.

**Fix in this review:** rewrite the line as:
```
metadata  object   Per-producer metadata tree.  Each key is a producer
                   role_uid (HEP-CORE-0033 §G2.2.0b); each value is that
                   producer's REG_REQ-supplied free-form JSON blob.
                   Producers with null metadata are omitted from the tree
                   (not present-as-null).  Empty tree is `{}`, never null.
                   See docs/tech_draft/controlled_access_api_design.md §6.1.
```

---

### F3 — HEP-CORE-0021 still describes single channel-level `zmq_node_endpoint`  ❌ OPEN

**Severity:** Blocker for step 3 — once the REG_REQ handler writes
endpoints per-producer (M2.5 step 3), the HEP wording becomes wrong.
**Files:**
- `docs/HEP/HEP-CORE-0021-ZMQ-Endpoint-Registry.md:600,702,749,757` —
  treats `zmq_node_endpoint` as a single channel-scope field.

**Problem:** The HEP says "the registered `zmq_node_endpoint`" (singular,
channel-scope) and the ENDPOINT_UPDATE_REQ wire description does not key
by producer uid. Post-step-2a + step 5 (ENDPOINT_UPDATE_REQ rewrite),
the endpoint registry is per-producer; the HEP must say so. ZMQ Fan-In
requires each producer to advertise its own bound endpoint.

**Fix in this review:** explicit "per-producer endpoint" wording in
HEP-CORE-0021 §2 (or wherever the registry semantics are introduced),
ENDPOINT_UPDATE_REQ section keyed by `(channel, role_uid)`.

---

### F4 — `ChannelSnapshotEntry` back-compat scalars are dead but still populated  ⚠️ STALE

**Severity:** Medium — exact bug class M2.5 is eliminating, still wired
up but with no consumer.
**Files:**
- `src/include/utils/broker_service.hpp:53-72` — declares scalar
  `producer_pid` / `producer_role_name` / `producer_role_uid` "for
  back-compat with single-producer admin clients."
- `src/utils/ipc/broker_service.cpp:3161-3166` — populates them from
  `entry.first_producer()`.

**Verified consumers:** grep across `src/` and `tests/` for direct reads
of `.producer_role_uid` / `.producer_role_name` / `.producer_pid` on a
`ChannelSnapshotEntry` instance returned **zero matches**. The fields
are dead.

**Effect:** Today the populated value is "the first producer." On a
Fan-In channel an admin client receiving the snapshot has no idea
multiple producers exist if it only reads the scalars. The
`producer_uids` / `producer_pids` vectors carry the truth — but the
scalars are a trap for any future reader who picks them up by
convenience.

**Fix in step 2c:** delete the three scalar fields from
`ChannelSnapshotEntry`; delete their population at
`broker_service.cpp:3162-3166`; keep `producer_uids` / `producer_pids`.
No tests need adjustment (verified — none read the scalars).

---

### F5 — Stale comments referencing pre-MP2 `channel.producer_role_uid`  ⚠️ STALE

**Severity:** Low — comment-only drift; misleads future readers.
**Files:**
- `src/utils/network_comm/broker_request_comm.cpp:635` — comment
  "channel.producer_role_uid as in pre-Phase-6"
- `src/utils/service/hub_api.cpp:260` — "channel's `producer_role_uid`"
- `src/utils/ipc/admin_service.cpp:392` — "the channel's
  producer_role_uid"
- `src/utils/ipc/broker_service.cpp:1434` — "(producer_role_uid, 'inbox')"
- `src/utils/ipc/hub_state.cpp:681` — "Validate every producer_role_uid
  in `entry.producers`" (half-correct — the validation is real,
  just the noun is dated)

**Fix in this review:** sweep — replace channel-scope references with
"the channel's producer-presence list" or
"`entry.producers[i].role_uid`" as appropriate.

---

### F6 — Field duplication during step 2a (transitional)  ⚠️ TRANSITIONAL

**Severity:** Medium — known and documented, but a real foot-gun until
step 3 closes the migration.
**Files:**
- `src/include/utils/hub_state.hpp` — `ChannelEntry.metadata` AND
  `ProducerEntry.metadata`; `ChannelEntry.zmq_node_endpoint` AND
  `ProducerEntry.zmq_node_endpoint` both exist.

**Problem:** Two fields with the same name, different semantics. A
caller writing `entry.metadata = blob` (channel-scope) instead of
`entry.set_producer_metadata(uid, blob)` (per-producer) compiles
silently. The comments around the new ProducerEntry fields say "STEP 2a
ADDITIVE — channel-scope copies retained until step 3", but anyone
landing in REG_REQ during step 3 implementation could miss the comment.

**Mitigation today (mediocre):** the design doc lists step 3 as the
caller migration. Code reviewer must check that step 3 commit deletes
the `ChannelEntry`-level versions.

**Fix in this review:** add a sharper deprecation comment on the
`ChannelEntry`-level `metadata` and `zmq_node_endpoint` fields —
something like `[[deprecated("step 2a transitional; use
set_producer_metadata / set_producer_zmq_node_endpoint")]]`. Actual
deletion lands in step 3.

---

### F7 — Dead placeholder fields `zmq_data_endpoint` / `zmq_ctrl_endpoint` / `zmq_pubkey` still live on `ChannelEntry`  ⚠️ STALE

**Severity:** Low — already flagged in the M2.5 design as step 2c
deletion target.
**Files:**
- `src/include/utils/hub_state.hpp:194-196` — declares the three.
- `src/include/utils/role_reg_payload.hpp:70-72` — producer-side
  hard-codes `tcp://127.0.0.1:0` / `""` placeholders.
- `src/utils/ipc/broker_service.cpp:1065-1067` — REG_REQ writes them.
- `src/utils/ipc/broker_service.cpp:1566-1568` — DISC_REQ_ACK echoes
  them.

**Verified:** no real values flow through these fields in any code
path. Placeholder-only.

**Fix in step 2c:** delete the three fields + the REG_REQ writes + the
DISC_REQ_ACK echoes + the producer-side placeholder strings. Update
HEP-CORE-0007 §12.4 to remove them from the wire schema.

---

### F8 — `_set_channel_opened` insert_or_assign blocks the M2.5 step 3 mechanism  ❌ BLOCKER (Step 3 design)

**Severity:** Critical — surfaces only when step 3 starts; flagging now.
**Files:**
- `src/utils/ipc/hub_state.cpp:322-331` — `_set_channel_opened` uses
  `pImpl->channels.insert_or_assign(entry.name, std::move(entry));`
- `src/utils/ipc/broker_service.cpp:1058-1438` — REG_REQ handler
  builds a fresh `ChannelEntry` and calls `_set_channel_opened`.

**Problem:** Today REG_REQ replaces the entire channel record. Once
step 3 migrates the broker handler to call
`ChannelEntry::add_producer`, the broker must instead **append** to
an existing channel's `producers[]` without disturbing the existing
record. There is no HubState op for "add a producer to an existing
channel" today. `_set_channel_opened` is the only entry point and it
overwrites.

**Fix needed in step 3 design (not this review's responsibility to fix,
but the gap must be recorded):** introduce
`HubState::_on_producer_added(channel_name, ProducerEntry)` that:
- if channel exists → calls `it->second.add_producer(...)` and surfaces
  the typed result.
- if channel does not exist → opens-then-adds atomically (single
  writer lock; emits `ch_opened` handler then iterates the
  add_producer result).

Or alternatively: extend `_set_channel_opened` to detect existing
channels and delegate to `add_producer`. Either works; the design
choice belongs to step 3 prep.

---

### F9 — Test coverage gaps for newly-added accessors  ⚠️ COVERAGE GAP

**Severity:** Low — the methods are simple; coverage is just thin.
**Files:** `tests/test_layer2_service/test_hub_state.cpp` — `ChannelEntryApi.*` block.

**Not directly tested:**
- `set_producer_inbox` (tested indirectly through ProducerEntry literal
  in `AddProducer_SameUidRejected_NoStateMutation`)
- `set_consumer_inbox` (no test)
- `set_producer_metadata` (tested indirectly via
  `aggregate_metadata_tree`)
- `producer_metadata(uid)` single-producer accessor (no test)
- `producer_zmq_node_endpoint(uid)` getter (no test — only setter)
- `remove_consumer` (no test)
- `is_shm()` (tested indirectly via SHM cardinality test)
- `producer_count()` / `consumer_count()` (tested indirectly)
- `find_consumer(uid)` (tested indirectly)

**Fix in this review:** add direct tests for these — one per accessor,
≤10 lines each. Pin path + structural payload per CLAUDE.md test-rigor
rule.

---

### F10 — `inject_orphan_role` test-access helper promised but not delivered  ⚠️ DEFERRED

**Severity:** Low — no test currently needs it; the promise lives in
the design doc.
**Files:**
- `docs/tech_draft/controlled_access_api_design.md` §6.5 — promises
  test-access helpers `force_presence_state`, `inject_orphan_role`.
- `tests/test_framework/hub_state_test_access.h` — exists; does not yet
  expose these.

**Fix:** record the deferral in this review and in
`docs/todo/TESTING_TODO.md`. The helpers will be added when M3 needs
them to set up "RoleEntry residue + new REG_REQ" scenarios. No fix
in this pass.

---

### F11 — Design doc §5.1 API methods not all delivered  ⚠️ DEFERRED

**Severity:** Low — intentional deferral; only worth flagging so it's
recoverable.
**Files:**
- `docs/tech_draft/controlled_access_api_design.md` §5.1 — listed
  `set_invariant_schema` / `set_invariant_transport` /
  `observable(roles_map)` / `is_alive(roles_map)` / `producers()` /
  `consumers()` span accessors.
- `src/include/utils/hub_state.hpp` — these are not yet present.

**Rationale for deferral:** `set_invariant_schema` needs a thoughtful
design pass on field-by-field semantics (which fields are atomic, what
match precision counts as IdempotentEqual). `observable(roles_map)`
exists as a free function; member wrapping is sugar. Span accessors
are sugar on top of the public vectors. Not needed for step 3.

**Fix:** add an explicit row in the design doc §7 step plan ("step 2d
— deferred API methods; lands after step 3 stabilises") and reference
this finding so the deferral is recoverable.

---

### F12 — `disconnected_fired` 🚧 PATCH still in source (known)  ⚠️ DEFERRED

**Severity:** Tracked.
**Files:**
- `src/include/utils/hub_state.hpp:644` — `RoleEntry.disconnected_fired`
- `src/utils/ipc/hub_state.cpp:399-455` — `_set_role_disconnected` uses it.

Already tracked for Wave M3 (RoleEntry API) per
`docs/tech_draft/controlled_access_api_design.md` §6.2. No action in
M2.5.

---

### F13 — `metrics_store_` is marked legacy but still in use  ⚠️ TRACKED

**Severity:** Tracked elsewhere.
**Files:**
- `src/utils/ipc/broker_service.cpp:245,1621,1990,3371,3534,3545,3609,3625`

Already tracked for retirement in M1.4 per `docs/TODO_MASTER.md` (after
M2.5 + MP3/4/5 close). No action in this audit.

---

### F14 — Test naming inconsistency  ⚠️ STYLE

**Severity:** Trivial.
**Files:** `tests/test_layer2_service/test_hub_state.cpp` — new tests
use `ChannelEntryApi` prefix; existing tests use
`HubStateSkeleton` / `HubStateOps` / `HubStateSchemas`.

The new tests pin a `ChannelEntry` member API, not a HubState op, so
the divergent prefix is *defensible*. Decided: keep `ChannelEntryApi`
prefix; document the convention so future ChannelEntry / RoleEntry API
tests follow the same shape. No code change.

---

### F15 — HEP-CORE-0033 §8 entry-types table needs per-producer field re-sweep  ❌ OPEN

**Severity:** Medium — keeps the docs accurate for §8 readers.
**Files:**
- `docs/HEP/HEP-CORE-0033-Hub-Character.md:716` — `ChannelEntry` row
  lists "data-plane endpoints" without specifying per-producer scope;
  `metadata` not mentioned.

**Fix in this review:** update the `ChannelEntry` row to reflect:
- producers[i] now holds inbox_*, zmq_node_endpoint, metadata
  (per-producer attributes per M2.5)
- ChannelEntry retains channel-wide invariants only (schema, transport,
  pattern, shm_name)
- Cross-link to `docs/tech_draft/controlled_access_api_design.md`.

---

## Status Summary

| ID | Title | Severity | Status |
|---|---|---|---|
| F1 | HEP-0007 §12.4a missing UID_CONFLICT; MULTI_PRODUCER entry contradicts strict reject | Blocker | ✅ FIXED 2026-05-10 |
| F2 | HEP-0007 §12.4 DISC_REQ_ACK metadata wire shape drift | Blocker | ✅ FIXED 2026-05-10 |
| F3 | HEP-0021 still describes single channel-level zmq_node_endpoint | Blocker | ✅ FIXED 2026-05-10 |
| F4 | ChannelSnapshotEntry back-compat scalars dead but populated | Medium | ✅ FIXED 2026-05-10 (step 2c) — three scalars + their population deleted; verified zero consumers in src/ + tests/ |
| F5 | Stale `channel.producer_role_uid` comments in 5 files | Low | ✅ FIXED 2026-05-10 |
| F6 | Field duplication during step 2a (transitional) | Medium | ✅ FIXED 2026-05-10 (DEPRECATED comments added) |
| F7 | Dead `zmq_data_endpoint` / `zmq_ctrl_endpoint` / `zmq_pubkey` fields | Low | ✅ FIXED 2026-05-10 (step 2c) — DESIGN-DRIVEN OUTCOME: `zmq_ctrl_endpoint` + `zmq_data_endpoint` truly dead (deleted; HEP-0017/0021 explain why — see design doc §3.1 rows).  `zmq_pubkey` is NOT dead (HEP-CORE-0021 §5.2 designs it as per-producer for ZMQ ctrl socket auth); moved to `ProducerEntry.zmq_pubkey` with new `set_producer_zmq_pubkey` / `producer_zmq_pubkey` API methods; channel-scope `ChannelEntry.zmq_pubkey` carries DEPRECATED comment until step 3 migrates the REG_REQ handler. |
| F8 | `_set_channel_opened` insert_or_assign blocks step 3 mechanism | Critical | ❌ OPEN (step 3 design pass next) |
| F9 | Test coverage gaps for accessors | Low | ✅ FIXED 2026-05-10 (step 2c) — 7 new ChannelEntryApi tests added: SetProducerInbox_KeyedByUid, SetConsumerInbox_KeyedByUid, SetProducerMetadata_KeyedByUid_AndProducerMetadataLookup, ProducerZmqNodeEndpoint_LookupAccessor, SetProducerZmqPubkey_PerProducerStorage, RemoveConsumer_PresentAndMissing, IsShmAndCounts_DerivedFromState.  Suite 1778 → 1785. |
| F10 | `inject_orphan_role` helper promised in §6.5, not delivered | Low | ⚠️ DEFERRED to M3 |
| F11 | Design doc §5.1 API methods (`set_invariant_*`, `observable(...)`) not delivered | Low | ✅ RECORDED 2026-05-10 (step 2d row in design doc §7) |
| F12 | `disconnected_fired` 🚧 PATCH still present | Tracked | ⚠️ DEFERRED to M3 |
| F13 | `metrics_store_` legacy still in use | Tracked | ⚠️ DEFERRED to M1.4 |
| F14 | Test naming convention | Trivial | ✅ KEPT |
| F15 | HEP-0033 §8 entry-types table needs per-producer re-sweep | Medium | ✅ FIXED 2026-05-10 |

## Items to fix in this audit pass

Land before step 3 (blocker class):
- F1 (HEP-0007 UID_CONFLICT)
- F2 (HEP-0007 metadata tree shape)
- F3 (HEP-0021 per-producer endpoint)
- F15 (HEP-0033 §8 entry-types refresh)

Land alongside the above:
- F5 (stale comment sweep — five files)
- F6 (deprecation comments on the duplicated fields)
- F11 (record the step-2d deferral in design doc)

Land in step 2c (next commit after this audit):
- F4 (delete dead back-compat scalars on `ChannelSnapshotEntry`)
- F7 (delete dead `zmq_data_endpoint` / `zmq_ctrl_endpoint` / `zmq_pubkey`)
- F9 (add the missing direct accessor tests)

Step 3 design:
- F8 (`_on_producer_added` or equivalent — choose mechanism)

Defer:
- F10, F12, F13 already tracked.

---

## Archival

This review will be archived to `docs/code_review/archive/` per
`docs/DOC_STRUCTURE.md §1.7` once all ❌ OPEN items are ✅ FIXED.
Cross-link from `docs/TODO_MASTER.md` "Active code review" until then.
