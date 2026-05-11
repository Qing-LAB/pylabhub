# Controlled-Access API Design — `ChannelEntry` + `RoleEntry`

| | |
|---|---|
| **Status**  | Draft — design phase. **No code changes yet.** |
| **Created** | 2026-05-10 |
| **Wave**    | M2 (MP2.5 + MP3) and M3 (RoleEntry follow-up) |
| **Drives**  | Eliminating the multi-producer / multi-presence "scalar-where-should-be-per-party" bug class structurally instead of fixing instances reactively. |
| **Resume point — return to main line of work** | After this design is approved and implemented (M2.5), the original Wave M2 phase plan resumes at **MP3 Bookkeeping ops** (see `docs/TODO_MASTER.md` "Wave M2"). MP3 → MP4 → MP5 are unchanged in *intent*, but their implementation rewrites every handler to go through this API surface instead of touching `ChannelEntry` fields directly. |

---

## 1. Why this exists

In three consecutive review passes during Wave M2 we found the same
bug class: a field declared at channel scope that should have lived
per-party. Concretely:

1. `ChannelEntry.producer_*` scalars (`pid` / `hostname` / `role_uid` /
   `role_name` / `zmq_identity`) — caught in MP2 pass 1.
2. `ChannelEntry`'s lack of `producers[]` cardinality reasoning in
   `observe` / fan-out — caught in MP2 pass 2.
3. `ChannelEntry.inbox_*` (4 fields) — caught in MP2 pass 3
   (committed at `91bd657`).

The **next layer** is wider than just `inbox_*`. The same overwrite
pattern applies to `zmq_node_endpoint`, `metadata`, and three dead
placeholder fields (`zmq_data_endpoint`, `zmq_ctrl_endpoint`,
`zmq_pubkey`). On top of that:

- REG_REQ re-registration discards the existing `producers[]` entirely
  by building a fresh `ChannelEntry` and calling `insert_or_assign`
  (`hub_state.cpp:327`).
- DEREG_REQ tears down the channel on any single producer's leave,
  ignoring HEP-CORE-0023 §2.1.1's "atomic teardown on **last**
  producer-presence Disconnected" rule.
- Zero tests register two distinct producers on the same channel
  (96 `register_channel` sites are all single-producer or same-uid
  restart). The scalar field design **prevented** anyone from writing
  such tests, so the bug class is hidden by absence.

The structural fix is a controlled-access API that encodes the
classification — "channel-wide invariant" vs "per-party attribute" —
in the type system, so future fields can't be misclassified by accident.

## 2. Scope

| Struct | Why it qualifies | Wave |
|---|---|---|
| **`ChannelEntry`** | 1..N producers + N consumers; channel-wide invariants (schema, transport) coexist with per-party fields (inbox, endpoint); coordinated teardown across parties | **M2.5** (this wave) |
| **`RoleEntry` / `RolePresence`** | 1..N presences per role uid; each presence runs its own FSM; per-uid liveness is derived (`any_presence_alive()`); in-flight `disconnected_fired` PATCH at `hub_state.hpp:339-360` is exactly the bug class this API would prevent | **M3** (next wave) |
| **`HubState.schemas_`** (schema registry) | Owner-keyed cascade lifecycle, path A/B/C/X dispatch | M4 (later — partial encapsulation already exists) |
| `PeerEntry` | Single party but has FSM transitions; same shape | Defer; pick up after M3 if pattern proves useful |
| `BandEntry` | Members join/leave; no per-member FSM | Defer; add only on concrete need |
| `ConsumerEntry` / `ProducerEntry` / `BandMember` | Leaf rows accessed through parent | No own API |

## 3. Field classification — `ChannelEntry`

Two-column model: every concrete field is either a **channel-wide
invariant** (one value per channel, all parties must agree) or a
**per-producer attribute** (lives on `ProducerEntry`). No third
category.

### 3.1 Channel-wide invariants

| Field | Reason | Validation rule on re-set |
|---|---|---|
| `name` | Identity. | Immutable post-create. |
| `shm_name` | SHM channels have one segment by physical constraint. | Immutable post-create. |
| `schema_hash` / `schema_version` / `schema_id` / `schema_blds` / `schema_owner` | All producers MUST agree (HEP-CORE-0023 §2.1.1, HEP-CORE-0034 §9.1). | Existing Cat-1 mismatch reject path; reuse. |
| `has_shared_memory` / `data_transport` / `pattern` | Channel topology choice; not per-producer. | Immutable post-create. |
| `created_at` | Captured on first REG. | Immutable. |

### 3.2 Per-producer attributes (move to `ProducerEntry`)

| Field | Current state | Action |
|---|---|---|
| `inbox_endpoint` / `inbox_schema_json` / `inbox_packing` / `inbox_checksum` | Already moved to `ProducerEntry` in `91bd657`. | Done. |
| `zmq_node_endpoint` | HEP-CORE-0021 endpoint registry — each Fan-In producer publishes from its own endpoint. Currently scalar on `ChannelEntry`. | **Move to `ProducerEntry`**. ENDPOINT_UPDATE_REQ keys by `(channel, role_uid)`. |
| `metadata` | Producer-supplied free-form JSON.  Decided 2026-05-10: per-producer (§6.1). | **Move to `ProducerEntry`** as `metadata` (nlohmann::json).  Wire shape on DISC_REQ_ACK: tree keyed by producer `role_uid` — see §6.1. |
| `zmq_ctrl_endpoint` | Originally (HEP-CORE-0002) the channel's ZMQ control endpoint.  Retired in current architecture: control plane is hub-mediated (HEP-CORE-0017 / HEP-CORE-0033); no per-channel ctrl endpoint exists.  No replacement at any scope. | **Delete** the channel-level field. |
| `zmq_data_endpoint` | Originally (HEP-CORE-0002) the channel's ZMQ data endpoint.  Function fully replaced by per-producer `ProducerEntry.zmq_node_endpoint` (HEP-CORE-0021 §16). | **Delete** the channel-level field. |
| `zmq_pubkey` | HEP-CORE-0021 §5.2 explicitly names this `producer_zmq_pubkey` and uses it in CONSUMER_REG_ACK for ZMQ ctrl socket auth — per-producer by design.  Currently mis-scoped on `ChannelEntry`; the wire layer sends empty string today but the propagation chain into `RoleEntry.pubkey_z85` exists. | **Move to `ProducerEntry`** as `zmq_pubkey` (step 2c).  Step 3 migrates REG_REQ handler to write to per-producer; step 3 also deletes the deprecated `ChannelEntry.zmq_pubkey`. |

## 4. Field classification — `RoleEntry` / `RolePresence`

`RoleEntry` already has the right shape (`presences[]` vector). The
unresolved item is the per-uid event-emit memoization currently
patched as `disconnected_fired` (line 339-360, marked 🚧 PATCH).

| Concern | Current home | M3 home |
|---|---|---|
| Per-presence FSM state | `RolePresence.state` / `.last_heartbeat` / `.state_since` | Same; correct shape. |
| Per-presence metrics | `RolePresence.latest_metrics` / `.metrics_collected_at` | Same. |
| Per-uid liveness | derived via `any_presence_alive()` | Same; correct. |
| `role_disconnected` event already emitted? | `RoleEntry.disconnected_fired` (PATCH) | Method on `RoleEntry`: `try_consume_disconnect_event() -> bool` — atomically returns true the first time it's called after the role transitions to all-presences-disconnected, false thereafter. Resets on first new presence add. |

## 5. API surface (proposed)

The design uses a **uniform pattern** so M2.5 (`ChannelEntry`) and M3
(`RoleEntry`) share the mental model. Methods live as `ChannelEntry` /
`RoleEntry` member functions; HubState op-helpers (e.g.,
`_on_channel_registered`, `_set_role_disconnected`) call them under
the writer lock. Public direct field access is removed by making the
state-bearing fields private (or `_private` by convention with
`friend HubState` and `friend test::HubStateTestAccess`).

### 5.1 `ChannelEntry` — proposed methods

Reading:

```cpp
const std::string& name() const noexcept;
const std::string& schema_hash() const noexcept;          // and the other invariants
const std::string& shm_name() const noexcept;
ChannelPattern     pattern() const noexcept;
std::string_view   data_transport() const noexcept;

// Per-party access:
const ProducerEntry*       find_producer(std::string_view uid) const noexcept;
const ProducerEntry*       first_producer() const noexcept;
std::span<const ProducerEntry> producers() const noexcept;     // for iteration only
std::span<const ConsumerEntry> consumers() const noexcept;
size_t                     producer_count() const noexcept;
size_t                     consumer_count() const noexcept;

// Derived:
ChannelObservable observable(const RolesMap&) const noexcept; // moved from free fn
bool              is_alive(const RolesMap&) const noexcept;   // any producer-presence live
```

Mutation (under writer lock; called only from HubState ops):

```cpp
// Channel-wide invariant set / re-set:
enum class InvariantSetResult { Created, IdempotentEqual, RejectedMismatch };
InvariantSetResult set_invariant_schema(std::string hash, std::string blds,
                                        std::string packing, std::string owner,
                                        std::string id);
InvariantSetResult set_invariant_transport(std::string data_transport,
                                            bool has_shm, ChannelPattern pattern);

// Per-producer admission — strictly additive, no restart-replace (§6.2).
enum class AddProducerResult {
    Created,                  // appended; entry pointer returned via out param
    RejectedUidConflict,      // a ProducerEntry with this role_uid already exists
                              // (regardless of liveness state) — bookkeeping
                              // residue or active producer; admission refused
    RejectedShmCardinality    // data_transport=="shm" + producers.size()>=1
};
AddProducerResult add_producer(ProducerEntry p, ProducerEntry** out = nullptr);

struct RemoveProducerResult { bool removed; bool channel_now_empty; };
RemoveProducerResult remove_producer(std::string_view role_uid);

// Per-producer field setters (keyed by uid; nullopt return = uid not found):
bool set_producer_inbox(std::string_view uid,
                        std::string endpoint, std::string schema_json,
                        std::string packing,  std::string checksum);
bool set_producer_zmq_node_endpoint(std::string_view uid, std::string endpoint);
bool set_producer_metadata(std::string_view uid, nlohmann::json blob);

// Read accessors for metadata (§6.1 decision — per-producer storage + tree
// aggregation on the channel-level read path):
const nlohmann::json* producer_metadata(std::string_view uid) const noexcept;
nlohmann::json        aggregate_metadata_tree() const;  // { "<uid>": blob, ... }

// Per-consumer — symmetric with add_producer (§6.2 uniform reject rule).
enum class AddConsumerResult {
    Created,
    RejectedUidConflict       // existing ConsumerEntry with this role_uid
                              // (residue or active) — admission refused
};
AddConsumerResult add_consumer(ConsumerEntry c, ConsumerEntry** out = nullptr);

bool remove_consumer(std::string_view role_uid);
bool set_consumer_inbox(std::string_view uid, /* same shape as producer */);
```

Direct write to fields like `entry.zmq_node_endpoint = req.value(...)`
becomes impossible — the only way is `entry.set_producer_zmq_node_endpoint(uid, ep)`.
A REG_REQ handler with the wrong shape fails to compile.

### 5.2 `RoleEntry` — proposed methods (M3)

```cpp
// Reading:
const RolePresence* find_presence(std::string_view channel,
                                  std::string_view role_type) const noexcept;
std::span<const RolePresence> presences() const noexcept;
bool any_presence_alive() const noexcept;     // (existing)

// Mutation — strict additive at the presence layer (a `RoleEntry`
// admits at most one presence per (channel, role_type) tuple; a
// re-add is bookkeeping residue, same class as §6.2):
enum class AddPresenceResult { Created, RejectedDuplicate };
AddPresenceResult add_presence(std::string channel, std::string role_type,
                               RolePresence** out = nullptr);
bool              remove_presence(std::string_view channel,
                                  std::string_view role_type);

// Transition primitives (called by sweep / heartbeat / dereg):
enum class TransitionEffect { NoChange, Refreshed, NewlyConnected, ToPending, ToDisconnected };
TransitionEffect on_heartbeat(std::string_view channel, std::string_view role_type,
                              std::chrono::steady_clock::time_point now);
TransitionEffect on_pending_timeout(std::string_view channel, std::string_view role_type);
TransitionEffect on_dereg(std::string_view channel, std::string_view role_type);

// Event-emit memoization (retires the `disconnected_fired` PATCH):
bool try_consume_disconnect_event() noexcept;
```

## 6. Open design decisions (must lock before code)

1. **`metadata` classification.**  **DECIDED 2026-05-10 — per-producer.**
   Rationale (user directive): the JSON shape means producers can publish
   orthogonal metadata blobs, and the channel API must organise them under
   each producer's key so a consumer sees the full set in one place.

   **Storage:** `ProducerEntry.metadata` (nlohmann::json, default `null`).
   Set via `set_producer_metadata(uid, blob)` from REG_REQ.

   **Wire shape on DISC_REQ_ACK** (channel-level read path —
   `aggregate_metadata_tree()`):
   ```json
   {
     "metadata": {
       "<producer_role_uid_1>": { ... producer 1's blob ... },
       "<producer_role_uid_2>": { ... producer 2's blob ... }
     }
   }
   ```
   A producer that did not supply metadata is omitted from the tree (its
   key is absent — not present-as-`null`).  An empty tree is `{}`, never
   `null`, so consumers can rely on the field being an object.

   **HEP work owed (M2.5 step 8):** update HEP-CORE-0007 §12.4 to spell
   out the tree shape; mark the old "free-form object at channel scope"
   wording as superseded.  This is a wire-shape change to DISC_REQ_ACK;
   it is free in practice because no producer-side or consumer-side
   caller currently uses the field (verified 2026-05-10 — only the
   broker writes/echoes it).

   **Same-uid: cannot happen.**  Per §6.2 below, any incoming REG_REQ
   carrying a uid that already exists in HubState is rejected with
   `UID_CONFLICT` — same-uid restart is not a normal pathway, so the
   metadata tree never sees a "slot replacement" by design.  Two
   producers (different uids) on the same channel may publish
   disagreeing metadata blobs without rejection — metadata is
   per-producer, not a channel-wide invariant.

2. **Same-uid REG_REQ — REJECT, always.**  **DECIDED 2026-05-10 (refined) —
   any incoming REG_REQ with a uid that exists in HubState is rejected
   with `UID_CONFLICT`, regardless of whether the existing entry is
   active or stale-residue.  Logged at `LOGGER_ERROR`.  Wire message:
   `"uid conflict, not trusting this connection, try again with clean state"`.
   No replace-in-place anywhere.**

   User directive (refined): "when a role is disconnected, its entry
   should be cleaned correctly.  When a new connection comes with
   conflicting information with the same uid (or even with the
   consistent information but also the same uid), that means
   something is wrong either on the remote side or on the
   bookkeeping side on the hub, and I think this should be rejected
   with clear message 'uid conflict, not trusting this connection,
   try again with clean state' and should be clearly logged —
   bookkeeping should not have conflicting uid if disconnection has
   happened.  If uid is still active then clearly it is an attempt
   to breach or the role is violating something."

   **Reasoning chain (uniform across the two interpretations):**
   - Existing entry is **stale-residue** → bookkeeping is in a bad
     state; cannot safely admit.  Operator/role must retry once the
     residue is cleared.
   - Existing entry is **active** → either spoofing attempt or
     role-side protocol violation; reject defensively.
   - There is no third "legitimate restart" case because proper
     uid construction (`tag.name.unique` per HEP-CORE-0033 §G2.2.0b)
     makes same-uid collision effectively impossible.

   **Symmetry rule:** the same policy applies to `CONSUMER_REG_REQ`.
   Current code dedups by uid/pid (`_add_consumer`, `hub_state.cpp:351-364`)
   — that becomes reject-with-`UID_CONFLICT`.  No silent replace
   anywhere in admission.

   **The cleanup side (M3) — current code has the residue bug:**
   - `_set_role_disconnected` (`hub_state.cpp:399`) marks all
     presences `Disconnected` but **never erases the `RoleEntry`**
     from `pImpl->roles`.  Confirmed via grep: zero `roles.erase`
     call sites anywhere in `src/`.
   - `upsert_role_locked` (`hub_state.cpp:619`) finds the existing
     entry and updates in-place — the residual `Disconnected`
     presences remain in `presences[]`.
   - Worse, `upsert_presence_row_locked` (line 588) returns early
     if the presence row already exists.  So same-uid re-register
     on the same channel **leaves the presence FSM stuck in
     `Disconnected`** — the new producer is alive but the FSM says
     it is dead.  This is a latent bug today.

   **Design call for M2.5 + M3:**
   - **M2.5** (`ChannelEntry`): `add_producer(p)` is purely additive.
     If `find_producer(p.role_uid) != nullptr` → return
     `RejectedUidConflict`; broker handler emits `LOGGER_ERROR` and
     replies with wire error `UID_CONFLICT`.  No replace path exists.
     (Note: name changed from `upsert_producer` to `add_producer` —
     "upsert" implied insert-or-update, which is exactly the
     restart-replace pattern this rule eliminates.)
   - **M3** (`RoleEntry`): `_set_role_disconnected` becomes a true
     terminal cleanup — when the role's last presence transitions to
     `Disconnected`, the `RoleEntry` is erased from `pImpl->roles`
     and any cascade evictions (schemas, presence rows) run before
     erase.  `add_role(uid)` finding any residual `RoleEntry` → return
     `RejectedUidConflict` (same shape as `add_producer`).
   - **MP3 implication:** the in-flight `disconnected_fired` 🚧 PATCH
     at `hub_state.hpp:339-360` retires naturally — once erase is
     the terminal operation, there is no "fire-twice" risk because
     there is no surviving entry to re-fire from.
   - **Test-access escape hatch** (per §6.5): tests that need to set
     up "channel with a Disconnected-residue presence before a fresh
     REG_REQ arrives" use `HubStateTestAccess::inject_orphan_role`
     to construct the residue state, then assert the REG_REQ rejects
     with `UID_CONFLICT`.  The reject path itself becomes a tested
     contract, not a comment.

3. **SHM physical constraint.**  **DECIDED 2026-05-10 — enforce in
   the API; SHM channels admit exactly one producer.**

   User directive: "the SHM is special — only one producer; API
   would not have any exposure to a second producer connecting to
   it.  Only consumer is allowed.  Check API to confirm."

   **Current code (verified 2026-05-10) does NOT enforce this:**
   - `broker_service.cpp:1063` (REG_REQ handler) reads
     `has_shared_memory` and `data_transport` from the payload but
     **performs no SHM-specific producer-cardinality check**.
     Confirmed via grep: zero matches for "shm" + "MULTI_PRODUCER" /
     "reject" / "single" near REG_REQ handling.
   - A second producer's REG_REQ on a SHM channel today passes the
     schema gate (same hash) and triggers the `_set_channel_opened`
     overwrite path described in §1 of this doc.  Protection exists
     only at the SHM data-plane (segment ownership), not at the
     broker control-plane.

   **Design call:** `ChannelEntry::upsert_producer(p)` rejects when
   `data_transport == "shm"` and `producers.size() >= 1` (and the
   incoming uid is not a same-uid restart, which is handled per §6.2
   above).  Returns a typed reject result; broker handler surfaces
   `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM` on the wire.

4. **Cross-tag admission.**  **DECIDED 2026-05-10 — allow any tag;
   document only.**

   User directive: "allow any tag, document only."  Per HEP-CORE-0017
   a processor's `out_channel` registers as a producer; `prod.X` +
   `proc.Y` may both be producers of channel `C`.  `upsert_producer`
   docstring documents the rule; HEP-CORE-0017 §pipeline-architecture
   cross-references.

5. **Test-access escape hatch.**  **DECIDED 2026-05-10 — keep the
   friend shim with documented mutation helpers.  Already satisfies
   the user's gating constraints by file location.**

   User directive: "choose option 1 to keep the friend shim, confirm
   that this will not compile into final product when WITH_TEST is
   set to false, and in addition, this is not exposed in public
   header."

   **Verified 2026-05-10:**
   - `HubStateTestAccess` is defined in
     `tests/test_framework/hub_state_test_access.h` — under
     `tests/`, NOT under `src/include/`.  Production headers contain
     only a forward declaration (`hub_state.hpp:563`) and the
     `friend struct ::pylabhub::hub::test::HubStateTestAccess;`
     line at `hub_state.hpp:640`.
   - The forward decl is a no-op when the struct is never defined
     in a TU (i.e., production builds that don't include the test
     header).  The friend statement compiles harmlessly without the
     struct being instantiated.
   - This file layout already satisfies both constraints: the shim
     does not link into the production library, and no production
     header exposes its surface.

   M2.5 extends the existing pattern with explicit per-API mutation
   helpers (e.g., `force_presence_state`, `inject_orphan_role`),
   each annotated `// test-only; production must use the public API`.

## 7. Migration plan — M2.5

| Step | Scope | Files | Tests |
|---|---|---|---|
| **0** | This design doc lands; field classification + open-decisions locked with user. | (this file) | — |
| **1** | Add the proposed methods to `ChannelEntry` (header-only inline where possible; .cpp for non-trivial ones). Fields stay public for now — additive change. | `src/include/utils/hub_state.hpp` (+`.cpp`) | Existing 1769 tests must still pass; no behaviour change. |
| **2** | Migrate the field migrations agreed in §3 (`zmq_node_endpoint` → per-producer; `metadata` per §6.1; delete dead `zmq_data_endpoint` / `zmq_ctrl_endpoint` / `zmq_pubkey`). | `hub_state.hpp`, `broker_service.cpp`, `hub_state_json.cpp`, HEP-CORE-0021 / HEP-CORE-0023 §2.6 / HEP-CORE-0033 §8 | Multi-producer L2 tests (write the FIRST one — two distinct producer uids on a ZMQ channel; assert endpoints are independent). |
| **3** | Rewrite REG_REQ handler to use `upsert_producer` + per-producer setters. Eliminate the build-fresh-entry-and-replace-channel pattern. | `broker_service.cpp:handle_reg_req` | L2: same-uid restart preserves channel topology; new-uid append preserves first producer. |
| **4** | Rewrite DEREG_REQ to call `remove_producer` and only close the channel when `channel_now_empty`. | `broker_service.cpp:handle_dereg_req` | L2: DEREG of A on A+B channel leaves B alive; DEREG of last producer tears channel down. |
| **5** | Rewrite ENDPOINT_UPDATE_REQ to scope by `(channel, role_uid)`. | `broker_service.cpp:handle_endpoint_update_req` | L2: A's update doesn't mutate B's endpoint. |
| **6** | Rewrite sweep / heartbeat-timeout paths to iterate `producers[]` and call `_on_producer_dropped` on the matched producer only. | `broker_service.cpp` sweep + heartbeat handlers | L2: A timeout / B alive ⇒ channel stays kLive (already in MP5 plan; promote here). |
| **7** | Make state-bearing fields private (or move into `Impl`). Compile-fail any remaining direct field write. | `hub_state.hpp` | Build must remain clean. |
| **2d** | Deferred API methods from §5.1 — `set_invariant_schema` / `set_invariant_transport` (need field-by-field invariant precision design pass; broker's Cat-1 schema check owns this contract for now), `observable(roles_map)` member wrapper (sugar over the free `compute_channel_observable`), `is_alive(roles_map)`, `producers()` / `consumers()` span accessors.  Lands AFTER step 3 stabilises — by then the broker handler's actual needs are clearer.  Tracked as `REVIEW_WaveM2.5_2026-05-10.md` F11. | `src/include/utils/hub_state.hpp` | TBD |
| **8** | HEP doc sweep.  Update HEP-CORE-0023 §2.6 (struct schematic), HEP-CORE-0021 (per-producer endpoint registry), HEP-CORE-0033 §8 (entry-type table) to reflect the final classification.  Update HEP-CORE-0007 §12.4 (DISC_REQ_ACK metadata wire shape — tree keyed by producer uid) and §12.4a Error Code Taxonomy (add new `UID_CONFLICT` code, both producer + consumer admission paths; add `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM` if not present).  Cross-reference between the HEPs and the new API surface.  **HEP doc-sweep partial pre-land:** HEP-CORE-0007 §12.4 metadata wire shape + §12.4a `UID_CONFLICT` + `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM` rewording; HEP-CORE-0021 §16 per-producer endpoint; HEP-CORE-0033 §8 entry-types table — all landed in `2fef5dd`+commit before step 3 because step 3 ships the wire-shape change (per `REVIEW_WaveM2.5_2026-05-10.md` F1/F2/F3/F15). | `docs/HEP/*.md` | — |

Steps 1-8 are sequenced — each must build and test clean before the
next. Multi-producer L2 tests (step 2) are written **first** and must
fail against the pre-M2.5 codebase, then pass after each step that
should make them pass; this is the contract lock.

## 7.5 Step 3 design — REG_REQ handler rewrite (F8)

Step 3 of the migration plan rewrites the broker's REG_REQ handler to
use `ChannelEntry::add_producer` instead of building a fresh
`ChannelEntry` and calling `_set_channel_opened` (which uses
`insert_or_assign`, overwriting any existing channel record).  The
rewrite has three concerns:

### 7.5.1 The HubState op

**Decision: introduce `HubState::_on_producer_added(channel_name, producer_entry, schema_fields)` as a new op.**

Today's `_on_channel_registered(entry)` semantics: "this entry IS the
channel now."  It cannot be used for additive admission because
internally it calls `_set_channel_opened` → `insert_or_assign` which
replaces the record.

Three options were considered (per `REVIEW_WaveM2.5_2026-05-10.md` F8):

| | Option |
|---|---|
| (A) | New op `_on_producer_added` that branches: existing channel → `add_producer` + `upsert_role_locked`; missing channel → build fresh + `_set_channel_opened` + `upsert_role_locked`. |
| (B) | Extend `_set_channel_opened` to detect existing channels and delegate. |
| (C) | Two-call pattern in the broker handler, taking the write lock externally. |

**Chosen: (A).**  Rationale:
- Explicit caller intent at the API name.
- `_set_channel_opened` keeps its existing "open new channel" meaning;
  changing it (option B) would mutate the semantic of the only entry
  point into the channels map, breaking the contract for every existing
  caller / test that relies on it.
- Option C leaks HubState's internal locking out to the broker handler,
  which is exactly what Wave M2.5 is trying to avoid.

### 7.5.2 Op signature

```cpp
struct ProducerAdmissionResult {
    AddProducerResult   producer_result;  // Created / RejectedUidConflict /
                                          //   RejectedShmCardinality
    InvariantSetResult  invariant_result; // Created / IdempotentEqual /
                                          //   RejectedMismatch
    bool                channel_opened;   // true if this admission was
                                          //   the first producer on the
                                          //   channel
};

ProducerAdmissionResult _on_producer_added(
    const std::string&             channel_name,
    ChannelEntry::SchemaInvariants schema_fields,   // schema_hash, blds,
                                                    //   version, id, owner
    ChannelEntry::TransportInvariants transport,    // has_shm, shm_name,
                                                    //   pattern, data_transport
    ProducerEntry                  producer);
```

Behavior:
1. Take writer lock on `pImpl->mu`.
2. If channel does not exist → create `ChannelEntry`, set invariants,
   `add_producer(producer)` (which is `Created` by construction), insert
   into `pImpl->channels`, set `channel_opened=true`,
   `invariant_result=Created`.
3. If channel exists → compare incoming invariants vs stored:
   - mismatch → `RejectedMismatch`, no state change, return.
   - match (`IdempotentEqual` / `Created` resolved) →
     `it->second.add_producer(producer)` and propagate the typed
     result (`Created` / `RejectedUidConflict` / `RejectedShmCardinality`).
   - If add_producer returned `Created`, also `upsert_role_locked` for
     the new producer's uid + fire role-registered handler.
4. Fire `ch_opened` handler ONLY when `channel_opened==true` (first
   producer); subsequent admissions don't re-open the channel.

`channel_opened` flag is what tells the caller whether to fire schema
record creation (path B), HEP-CORE-0034 schema-record bookkeeping, etc.
— those are per-producer-on-first-admission concerns.

### 7.5.3 Broker REG_REQ handler shape (target)

Today:
1. Parse REG_REQ payload.
2. Build a fresh `ChannelEntry entry`.
3. Schema mismatch check against existing channel (Cat-1).
4. HEP-CORE-0034 schema record path A/B/C.
5. `hub_state_->_on_channel_registered(std::move(entry))`.

After step 3:
1. Parse REG_REQ payload.
2. Build a `ProducerEntry producer` (the new admission only).
3. Build `SchemaInvariants` + `TransportInvariants` from payload.
4. Call `hub_state_->_on_producer_added(channel_name, schema, transport, std::move(producer))`.
5. Inspect the typed result:
   - `RejectedUidConflict` → wire-error `UID_CONFLICT` + LOGGER_ERROR.
   - `RejectedShmCardinality` → wire-error `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM`.
   - `RejectedMismatch` (invariants) → wire-error `SCHEMA_MISMATCH` (or
     `TRANSPORT_MISMATCH` if it's a transport field — refine the error
     code per which invariant mismatched).
   - `Created` (producer) → emit any per-admission notifications
     (e.g., HEP-CORE-0034 schema record creation if `channel_opened`).
6. The deprecated `ChannelEntry.metadata` / `.zmq_node_endpoint` /
   `.zmq_pubkey` channel-scope fields stop being written **from the
   REG_REQ path** in step 3.  They are NOT deleted in step 3 because:
   - `_on_channel_registered` is retained as test-only legacy
     primitive (§7.5.4); it still reads `entry.zmq_pubkey` to seed
     `RoleEntry.pubkey_z85` and would break if the channel-scope
     field disappears.
   - `_set_channel_zmq_node_endpoint` is called by ENDPOINT_UPDATE_REQ
     handler — that migration is step 5, not step 3.
   - `ChannelEntry.metadata` still appears in DISC_REQ_ACK echo at
     `broker_service.cpp:1565`; that emission becomes incorrect once
     REG_REQ stops feeding it (returns the per-producer tree shape
     decided in §6.1) — fix in step 3 or defer to step 5 dispatch.
   The per-producer setters (`set_producer_metadata` /
   `set_producer_zmq_node_endpoint` / `set_producer_zmq_pubkey`) are
   called inside `_on_producer_added` from the producer payload —
   the producer fields are part of the `ProducerEntry` argument.

   **Step 3 DISC_REQ_ACK responsibility (decided):** rewrite the
   metadata echo at `broker_service.cpp:1565` to emit
   `aggregate_metadata_tree()` (the per-producer tree decided in
   §6.1) — this closes the wire-shape contract the HEP doc-sweep
   landed in commit `25dc376`.  The `zmq_node_endpoint` + `zmq_pubkey`
   echoes stay as-is (first-producer transitional shape) until
   step 5 (ENDPOINT_UPDATE_REQ rewrite) lands the per-producer wire
   shape for those too.

7. **Channel-scope field deletion deferred to a dedicated "step 6.5"
   sub-pass** (not step 3) — only safe AFTER step 5 retires
   `_set_channel_zmq_node_endpoint` AND step 3's test-side migration
   of `_on_channel_registered` to read per-producer pubkey lands.
   F6 "field duplication" risk remains visible (`[[deprecated]]`-style
   comments in place) until then.  Tracked.

### 7.5.4 Deletion of `_on_channel_registered`?

Currently `_on_channel_registered` is the ONLY caller of
`_set_channel_opened` outside test scaffolding.  Once the broker
REG_REQ migrates to `_on_producer_added`, `_on_channel_registered` has
no production callers.  Two choices:
- Keep it for test-access (existing L2 tests use it via
  `HubStateTestAccess`).
- Delete it; update tests to use `_on_producer_added` via test-access.

**Recommendation: keep for now** as a test-access primitive (it's
private; the public API surface doesn't change).  Mark in a comment as
"test-only legacy entry."  Step 7 (privatize fields, finalize API) can
revisit deletion after the broker handler is stable.

### 7.5.5 Test plan

Each new admission scenario gets an L2 test against
`_on_producer_added` (via `HubStateTestAccess`):
- First producer on a fresh channel → `Created` / channel_opened=true,
  consumers preserved if there were existing consumers (edge case, rare
  but possible if a consumer registered before any producer — should be
  rejected as CHANNEL_NOT_FOUND elsewhere; verify).
- Second producer on an existing channel, distinct uid, matching
  invariants → `Created` / `IdempotentEqual` for invariants /
  channel_opened=false.
- Second producer, distinct uid, schema-hash mismatch →
  `RejectedMismatch` (invariants).
- Second producer, same uid as existing → `RejectedUidConflict`.
- Second producer on SHM channel → `RejectedShmCardinality`.

Each test asserts (1) the typed result, (2) channel state after (no
partial mutation on reject), (3) consumer list preserved across the
admission.

## 8. Return to main line — what M2.5 unblocks

Once M2.5 lands, the rest of Wave M2 from `docs/TODO_MASTER.md`
resumes:

- **MP3** — Bookkeeping ops (HubState). Implementation becomes
  trivial because `_on_producer_dropped` is just `remove_producer` +
  conditional `_on_channel_closed`; `_set_role_disconnected` becomes
  iterate-and-call. The `disconnected_fired` PATCH retires.
- **MP4** — Broker handlers. Most of MP4 is already accomplished by
  M2.5 steps 3-6 (REG_REQ, DEREG_REQ, ENDPOINT_UPDATE_REQ, sweep).
  Remaining MP4 work: ROLE_INFO_REQ / ROLE_PRESENCE_REQ /
  CHANNEL_ERROR_NOTIFY / CHANNEL_CLOSING_NOTIFY fan-out.
- **MP5** — Tests. Multi-producer L2 + L3 coverage. Some of this lands
  alongside M2.5 steps 2-6 (locks contracts); the rest is end-to-end
  L3 scenarios.
- **MP6** — Federation/dual-hub (Wave B M8). Unchanged.

Wave M3 (`RoleEntry` API per §5.2) follows MP3-MP5 closure. The
shared design pattern means M3 is mostly mechanical.

## 9. Risks / non-goals

- **Not** a rewrite of HubState. The class stays; the API gains
  encapsulation at the entry-struct level.
- **Not** a wire-protocol change. REG_REQ / DEREG_REQ / heartbeat
  payloads are unchanged; only the broker-side bookkeeping shape
  changes.
- **Not** a federation change. MP6 / Wave B M8 / HEP-CORE-0022 still
  follow the same plan.
- **Risk:** 66+ call sites get refactored. Mitigation: phased
  migration in §7 keeps tests green at every step. Steps 1-2 are
  additive; steps 3-7 are per-handler.
- **Risk:** test-access escape hatch could become a back-door that
  re-introduces the bug class. Mitigation: confine test-access to
  `test::HubStateTestAccess` and require any new mutation helper there
  to be documented as "test-only; production must use the public API".
