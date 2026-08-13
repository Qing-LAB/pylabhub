# Peer rows on the wire — one shape, one channel

> **Status:** COMPLETE, 2026-08-11.  Tracked as task **#145**.
> Contract amended, all writers and all readers on one channel, L1 pins
> added.  Suite green at 2797/2797.
>
> The reader inventory in this document was wrong three times before it
> was right — 3, then 6, then 1, and the true answer was **4**.  §7 now
> records what the miscounts were and why, because the shape of the
> mistake is more useful than the number.

---

## 1. The problem in one paragraph

A producer's script asked *"is consumer X allowed on my channel?"* and got
**false** while X was admitted and sending data.  The channel's peer list
had the right number of entries; the entries just had no names in them.
The name was missing because the list is built in several places, and only
some of them filled it in.

---

## 2. Why it happened

Several messages carry "the peers on this channel".  They differ only in
*which* peers and whether the reader is going to dial them.  But each was
built by its own hand-written loop, and the loops disagreed:

| Builder | Source it reads | What it emitted |
|---|---|---|
| Producer reply, dialing branch | channel snapshot | name + key + endpoint |
| Producer reply, listening branch | admission ledger | **key only** |
| Consumer reply | channel snapshot | name + key + endpoint |
| Allowlist refresh | admission ledger | **a bare string** |

The admission ledger stores only keys — that is correct, it is the ZAP
enforcement set.  The mistake was that the two builders reading it never
asked anything for the names, while the two reading a snapshot got names
for free.  A role therefore held rows with a blank name column, and
`allowed_peer_contains(channel, uid)` compares that column.

**The hub always knows the pair.**  A key reaches a channel's admission
ledger only by passing the registration gate, which proves the key against
the CURVE handshake and resolves it to its roster owner before admitting
it.  Verified in code:

- `PeerAuthority::check_registration_claim` — refuses `pubkey_mismatch`
  when the announced key differs from the handshake-proven key.
- `PeerAuthority::check_role_ownership` → `attribute_sender` — refuses
  `identity_mismatch` when the claimed uid is not that key's owner.
- `HubState::_on_channel_peer_admitted` → `ledger.admit(pubkey)` — only ever
  receives a key that passed those gates.

So a nameless admitted key is an **invariant break**, never a normal case.

---

## 3. TWO CHAINS — do not conflate them

This is the single most important thing to carry forward.  I got it wrong
twice.  Two different chains share a row shape and are otherwise unrelated:

```
  ALLOWLIST CHAIN — "who may connect TO me"          (a membership SET)
  ───────────────────────────────────────────────────────────────────
   producer registers
     broker ──► REG_ACK.initial_allowlist
       role  ──► allowlist_cache ──► ZAP        (enforcement)
       role  ──► script view      ──► allowed_peer_count / _contains
   membership changes
     broker ──► GET_CHANNEL_AUTH_ACK.allowlist  (refresh — REPLACES the set)


  ATTACH CHAIN — "who I should connect TO"           (a DIAL TARGET)
  ───────────────────────────────────────────────────────────────────
   consumer registers
     broker ──► CONSUMER_REG_ACK.producers[]
       role  ──► endpoint + pubkey ──► dial the producer
```

Opposite directions.  The allowlist is a set the role **enforces against
inbound peers** and which each message **replaces wholesale**.  The attach
list is **where to reach someone**, consumed once.

**Consequence for this work:** only the allowlist chain gets
`parse_peer_list` (all-or-nothing replacement).  Applying it to the attach
chain would impose membership semantics on something that is not a
membership set — a mechanical substitution that would look tidy and be
wrong.

---

## 4. The contract (already amended — do not re-litigate)

**HEP-CORE-0036** is the owner.  Both edits are shipped:

| Field | Was | Now |
|---|---|---|
| `initial_allowlist` element | `{role_uid?, endpoint?, pubkey_z85}` — name "optional metadata" on the binding side | `{role_uid, endpoint?, pubkey_z85}` — **name required on every topology**; only `endpoint` varies |
| `GET_CHANNEL_AUTH_ACK.allowlist` | `array<string>`, bare Z85 | the same row as the seed |

New subsection **"Why a peer entry always names its peer"** carries the
reasoning, including the two consequences: never emit bare strings, and
the seed and refresh must carry the same shape (a mismatch is correct
until the first membership change, which is the worst way to be wrong).

**HEP-CORE-0046** gained one authoring rule: *type the ROW, not just the
list.*  A typed accessor that returns a raw array guards only the bracket;
every row inside is unowned, which is exactly how the two ends drifted
while both passed the guard.

Supporting rule already in **HEP-CORE-0035 §4.9** (cite, do not restate):
a receiver cannot be asked to name a sender from a list that never told it
any names.

---

## 5. The design

```
   ledger (keys only) ──┐
                        ├──► PeerListBuilder ──► PeerRow ──► the wire
   snapshot (pairs)  ───┘       · names keys via PeerAuthority
                                · sorts by uid
                                · logs AdmittedKeyHasNoName
                                · PeerDetail decides endpoint
```

- **`wire::PeerRow`** (`src/include/utils/wire_bodies.hpp`, impl in
  `src/utils/network_comm/wire_bodies.cpp`) — `from_pair`, `parse`,
  `to_json(PeerDetail)`.  **No constructor omits the name**, so a nameless
  row is unconstructible rather than merely discouraged.
- **`wire::PeerDetail`** — `IdentityOnly` | `WithEndpoint`.  The only axis
  on which these lists legitimately differ, so it is an argument rather
  than a second loop.  It appears on **both** ends: `to_json(detail)` says
  what the writer emits, `parse(entry, detail)` says what the reader
  needs.  A reader that will dial states so and is refused a row with no
  endpoint, at parse time rather than at connect time.
- **What a valid row is lives in one place.**  `PeerRow::parse` judges the
  key with `Z85PublicKey::try_validate` — the one authority on that
  question (HEP-CORE-0040 §8.4.1) — instead of the length test that used
  to be written out at the queue and nowhere else.  A reader with its own
  weaker rule accepts rows the others refuse, which is the original
  divergence one layer down.
- **`wire::parse_peer_list`** — the one read for a whole list,
  all-or-nothing.  `nullopt` means keep what you had.  Partial acceptance
  is refused on purpose: these lists replace the reader's set, so a
  dropped row is a peer silently denied.  An **empty** list is valid and
  returns an empty vector — a fresh channel has no peers, and treating
  empty as unusable preserves a stale set.
- **`PeerListBuilder`** (file-local in `src/utils/ipc/broker_service.cpp`)
  — the naming half.  Broker-side because only the broker holds the
  authority.

### Validity is shared; disposition is the caller's

The row type answers "is this a peer row".  What to DO with a bad one is
the caller's, and the two answers in the tree are both correct:

| Caller | On a bad row | Why |
|---|---|---|
| Seed, refresh, queue | refuse the whole field | the list REPLACES a set; the readable part is a quietly smaller allowlist |
| Consumer's `producers[]` projection | skip it, keep going | HEP-CORE-0042 §7.1 — one peer must not strand the batch; and the view is filtered to confirmed producers anyway, so a skipped row was never one it would have kept |

Writing this down matters because the two sites now look inconsistent to
anyone reading them cold, and "fixing" either one breaks a contract.

### Concurrency rules (verified — keep them)

- `PeerAuthority` has **zero mutexes** — immutable by construction, so
  calling it inside `ledger.for_each_admitted` (which runs under the
  HubState mutex and forbids re-entrant ledger calls) nests no lock.
- Hold `shared_ptr<const PeerAuthority>` for the whole build, grabbed
  **before** any channel state.  A bare reference can dangle once roster
  reload lands (#89) because the authority is replaced by pointer swap.
  Pattern already used by `roster_ack_block`.
- Do **not** copy the authority object — it is a map of every known role,
  and this is a per-REG_ACK path.
- Sort at the wire boundary; the ledger's iteration order is explicitly
  unspecified.

---

## 6. What is DONE

| Area | Change |
|---|---|
| Contract | HEP-CORE-0036 §6.2 + §6.5 amended; "What counts as a well-formed row" added; HEP-CORE-0046 row-typing rule added |
| Lookup | `PeerAuthority::local_uid_for_key` — one question, no roster handed out; kind test reused so a federation key is not named as a local role |
| Writers (**all 4**) | seed/dialing, seed/listening, consumer reply, refresh — all through `PeerListBuilder` |
| Readers (**all 4**) | seed, refresh, the queue's `apply_master_approval`, and the consumer's `producers[]` → script-view projection |
| Key rule | `PeerRow::parse` uses `Z85PublicKey::try_validate`; the queue's local `size() != 40` test is gone |
| Headers | `hub_queue.hpp` and `hub_zmq_queue.hpp` both said the PUSH side reads `artifacts["allowlist"]`.  It reads `initial_allowlist`; `allowlist` is the refresh field on a different message |
| Tests | one-to-one + fan-in topology assertions; two Pattern-4 wire tests moved to the row shape; **12 new L1 pins** for `PeerRow` / `parse_peer_list` in `tests/test_layer1_base/test_wire_envelope.cpp` |

The L1 pins were missing entirely — a new wire type shipped with its
behaviour tested only through the two roles that happened to use it.  They
lock the shape, both refusals, the endpoint axis, all-or-nothing, and that
an empty list is valid.

---

## 7. The reader count was wrong three times

Worth more than the fix.  The count went **3 → 6 → 1 → 4**, and each
answer was produced by looking harder at the same code.

| Miscount | What caused it |
|---|---|
| 3 | first pass, incomplete grep |
| 6 | counted the attach-chain sites as membership readers because they read a field with the same row shape |
| 1 | corrected the over-count by excluding attach sites — and excluded one site that was BOTH |
| **4** | the site at `role_api_base.cpp` ~1518 reads the attach message and projects it into the MEMBERSHIP cache.  Its source is the attach chain; its destination is `allowlist_cache`, which is what `allowed_peer_contains` answers from |

The lesson is not "grep harder".  It is that **a site belongs to the chain
of its DESTINATION, not the chain of the field it reads**.  Every miscount
came from classifying by source field.

That fourth site had the original defect still live: it accepted a row
with an empty `role_uid` (it guarded only the key) and skipped malformed
rows silently.  On the ZMQ branch the later filter happened to drop such
rows; **on the SHM branch, which does no filtering, a nameless row reached
the script cache**.

---

## 8. What is LEFT

### 8.1 Fan-out has no L4 test — task **#146**

`tests/test_layer4_plh_hub/test_plh_hub_role_zmq_e2e.cpp` covers
one-to-one and fan-in.  There is **no fan-out e2e at all**.  Fan-out is the
multi-entry binding case — the one that would catch a row bug that only
appears with more than one peer.

(An earlier revision of this document cited **#144** here.  That is a
different item — the peer/band inquiry engine coverage gap.  The fan-out
gap had no task at all until #146 was filed.)

### 8.2 Do NOT migrate these (attach chain — §3)

Both DIAL.  Neither feeds a membership view, which is the test that
matters (§7): classify by destination, not by the field being read.

- `role_api_base.cpp` ~1670, in `apply_consumer_reg_ack` — SHM branch
  reads `producers[0]` for endpoint + pubkey to dial a capability
  transport.  Branches on **transport**, never topology, so it does not
  trip the §I9.1 guardrail.  `producers[0]` is justified: SHM is
  single-producer per HEP-CORE-0023 §2.1.1, enforced at REG_REQ.
  Bypassing `apply_master_approval` here is a documented designer
  decision (HEP-CORE-0036 §5b note).
- `role_api_base.cpp` ~1826 — the ZMQ pre-attach walk, which issues one
  `CONSUMER_ATTACH_REQ_ZMQ` per producer and narrows `producers[]` to the
  ones that confirmed.  HEP-CORE-0042 §7.1 states this loop always runs to
  completion, so it must keep skipping bad rows rather than refusing the
  batch — the opposite disposition to the allowlist readers, on purpose.

---

## 9. How to verify

```bash
# build (never two cmake at once; -j2)
cmake --build build --target stage_all -j2

# the row contract itself
tools/ctest_evidence.sh -R "WirePeerRow|WirePeerList"

# the two topology tests that pin the end-to-end behaviour
tools/ctest_evidence.sh -R "ZmqE2E_AuthorizedConsumerReceivesAllSlots|ZmqE2E_MultiProducer_TwoAuthorized"

# closing a lib-contract change requires the full unfiltered sweep
tools/ctest_evidence.sh -j 2
```

Never raw `ctest`; never `--rerun-failed` without reading the preserved
log first.  Expected: **2797 tests, 100% pass** (4 skipped — federation
and one ABI case, unrelated).

The behavioural pin is a log marker emitted by a real Python script in the
L4 e2e:

```
prod_test: allowlist count=1 has_peer=True has_ghost=False
```

`has_ghost` must be **False** — without it, an accessor that returned true
for everything would satisfy `has_peer` and look correct.

---

## 10. Lessons worth keeping

- **The bug was invisible to counting.** The row existed; only a column
  was blank.  Any test asserting list size passed throughout.
- **The only pre-existing test touching this surface asserted the list was
  EMPTY** (no auth wired).  It would have passed with the feature deleted.
- **Topology exposed the branch.** Fan-in passed while one-to-one failed,
  same script and assertion — which localised the defect to one builder
  without bisection.  Per-topology pinning is not decoration here.
- **Deriving the assertion from the code would have enshrined the bug.**
  Running it first and recording the output would have written
  `has_peer=False`.
- **Classify a site by where its data GOES.** Every wrong reader count
  came from classifying by the field being read.  Two sites read the same
  array; one dials from it and one answers the script's membership
  question, and only the second is bound by the membership rule.
- **A new type with no direct test is not finished.** `PeerRow` shipped
  tested only through the roles that used it; the pins that state its
  contract came later, in this pass.
- **Static reading of this surface has been wrong repeatedly** — six
  times across two sessions, each time changing the answer.  Read the
  whole function, follow the destination, then confirm at runtime.

---

## 11. Sources

| Topic | Where |
|---|---|
| Wire shape + the naming rule | `docs/HEP/HEP-CORE-0036-*.md` §6.2, §6.5, "Why a peer entry always names its peer" |
| Row-typing authoring rule | `docs/HEP/HEP-CORE-0046-*.md` § "Adding a field that is a LIST of things" |
| Why lists carry pairs | `docs/HEP/HEP-CORE-0035-*.md` §4.9 + `RosterEntry` doc in `pubkey_origin.hpp` |
| Identity binding at admission | `src/include/utils/security/pubkey_origin.hpp`; impl `src/utils/security/pubkey_origin.cpp` |
| Layer invariant (topology stays in the queue) | `tools/check_layer_invariant.sh`; HEP-CORE-0036 §I9.1 |
| Fan-in cardinality | `ChannelTopology` in `hub_state.hpp`; `check_cardinality` in `hub_state.cpp` |
